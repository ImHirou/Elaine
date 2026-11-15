#include "world/dimension.hpp"

#include <algorithm>
#include <new>

#include "world/chunk_codec.hpp"

#include PROTO_GEN_DIMENSION_TYPES_HPP

namespace world {

namespace {

namespace dim = proto::gen::dimension;

// Chunk is a fixed-size block: 24 sections, 9-bit column maps. Check the datapack's dimension types
// against that at compile time, so a version with a taller world fails the build, not a client.
consteval bool dimension_types_fit() {
    for (const dim::Type& t : dim::kTypes) {
        if (t.height < 16 || t.height / 16 > static_cast<int>(kMaxSections) || t.min_y % 16 || t.height % 16) return false;
        // Heightmap: ceillog2(height + 1) bits; ChunkSkyLightSources: ceillog2(height + 2).
        if (std::bit_width(static_cast<unsigned>(t.height)) != kHeightmapBits) return false;
        if (std::bit_width(static_cast<unsigned>(t.height + 1)) != kHeightmapBits) return false;
    }
    return true;
}
static_assert(dimension_types_fit(), "a dimension type does not fit world::Chunk (kMaxSections / kHeightmapBits)");

// Heightmap.update() for one map. `mask`/`want` select the map's predicate over state flags.
bool update_column(ColumnMap& map, const Chunk& c, i32 min_y, u32 lx, i32 y, u32 lz, BlockState s, u32 mask, u32 want) {
    const u32 i = lx + lz * 16;
    const i32 first = static_cast<i32>(map.get(i)) + min_y;  // getFirstAvailable
    if (y <= first - 2) return false;
    if ((flags_of(s) & mask) == want) {
        if (y >= first) {
            map.set(i, static_cast<u32>(y + 1 - min_y));
            return true;
        }
    } else if (first - 1 == y) {  // the column's top block went away: find the next one down
        for (i32 yy = y - 1; yy >= min_y; --yy) {
            const BlockState b = c.sections[(yy - min_y) >> 4].get(lx, static_cast<u32>(yy & 15), lz);
            if ((flags_of(b) & mask) == want) {
                map.set(i, static_cast<u32>(yy + 1 - min_y));
                return true;
            }
        }
        map.set(i, 0);
        return true;
    }
    return false;
}

}  // namespace

Dimension::Dimension(u32 type_id, std::string name, ChunkGenerator& gen, i32 compression_threshold)
    : type_id_(type_id),
      name_(std::move(name)),
      gen_(gen),
      threshold_(compression_threshold),
      min_y_(dim::kTypes[type_id].min_y),
      max_y_(dim::kTypes[type_id].min_y + dim::kTypes[type_id].height - 1),
      sections_(static_cast<u32>(dim::kTypes[type_id].height) / 16),
      has_sky_(dim::kTypes[type_id].has_skylight),
      light_(map_, LightEngine::Layout{min_y_, min_y_ >> 4, sections_, has_sky_}) {}

Dimension::~Dimension() {
    map_.for_each([](Chunk& c) { std::free(c.frame); });
}

// ---------------------------------------------------------------------------- pipeline

Chunk& Dimension::load(i32 cx, i32 cz) {
    if (Chunk* have = map_.find(cx, cz)) return *have;
    Chunk* c = new (chunk_pool_.alloc()) Chunk();
    c->x = cx;
    c->z = cz;
    c->sections_count = static_cast<u8>(sections_);
    c->light_count = static_cast<u8>(sections_ + 2);
    gen_.generate(*c, alloc_);
    prime_heightmaps(*c);
    if (has_sky_) light_.fill_sky_sources(*c);
    map_.insert(c);
    light_.init_chunk(*c);
    return *c;
}

Chunk& Dimension::lit(i32 cx, i32 cz) {
    Chunk* c = map_.find(cx, cz);
    if (c && c->status == ChunkStatus::Lit) return *c;
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) load(cx + dx, cz + dz);
    c = map_.find(cx, cz);
    light_.light_chunk(*c);
    return *c;
}

Chunk& Dimension::ticking(i32 cx, i32 cz) {
    Chunk* c = map_.find(cx, cz);
    if (LIKELY(c && c->ticking)) return *c;
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) lit(cx + dx, cz + dz);
    c = map_.find(cx, cz);
    c->ticking = true;
    return *c;
}

std::span<const u8> Dimension::frame(i32 cx, i32 cz) {
    // A block changed earlier this tick may still be waiting for its light: a frame built now would
    // carry the new block with the old light, and nothing would ever correct it for a viewer who
    // is not on the chunk's border. (Vanilla holds the chunk back on the light engine's future.)
    if (UNLIKELY(light_.has_work())) update_light();
    Chunk& c = ticking(cx, cz);
    if (UNLIKELY(!c.frame_valid)) encode(c);
    return {c.frame, c.frame_len};
}

void Dimension::encode(Chunk& c) {
    // The frame is rebuilt rarely and read by many: the moment to squeeze the chunk.
    for (u32 si = 0; si < sections_; ++si) c.sections[si].compact(alloc_);
    light_.fold_layers(c);
    scratch_.clear();
    write_chunk_packet(scratch_, threshold_, c);
    frame_bytes_ += scratch_.len;
    frame_bytes_ -= c.frame_len;
    u8* f = static_cast<u8*>(std::realloc(c.frame, scratch_.len));
    if (UNLIKELY(!f)) std::abort();
    std::memcpy(f, scratch_.data, scratch_.len);
    c.frame = f;
    c.frame_len = scratch_.len;
    c.frame_valid = true;
}

// Heightmap.primeHeightmaps for the three client heightmaps: highest block per column matching each
// map's predicate, stored as (y + 1 - min_y), 0 when the column has none.
void Dimension::prime_heightmaps(Chunk& c) {
    c.world_surface.fill(0);
    c.motion_blocking.fill(0);
    c.motion_blocking_no_leaves.fill(0);
    u8 need[256];
    std::memset(need, 7, sizeof need);
    u32 remaining = 3 * 256;
    BlockState states[Section::kVolume];
    for (i32 si = static_cast<i32>(sections_) - 1; si >= 0 && remaining; --si) {
        const Section& s = c.sections[si];
        if (s.only_air()) continue;
        s.unpack(states);
        for (i32 ly = 15; ly >= 0 && remaining; --ly) {
            const BlockState* layer = states + ly * 256;
            const u32 h = static_cast<u32>(si * 16 + ly + 1);
            for (u32 col = 0; col < 256; ++col) {
                const u32 n = need[col];
                if (!n) continue;
                const u32 f = flags_of(layer[col]);
                const u32 hit = n & (((f & state::kIsAir) ? 0u : 1u) | ((f & state::kMotionBlocking) ? 2u : 0u) |
                                     ((f & state::kMotionBlockingNoLeaves) ? 4u : 0u));
                if (!hit) continue;
                if (hit & 1) c.world_surface.set(col, h);
                if (hit & 2) c.motion_blocking.set(col, h);
                if (hit & 4) c.motion_blocking_no_leaves.set(col, h);
                need[col] = static_cast<u8>(n & ~hit);
                remaining -= static_cast<u32>(std::popcount(hit));
            }
        }
    }
}

void Dimension::update_heightmaps(Chunk& c, u32 lx, i32 y, u32 lz, BlockState s) {
    // Same order as LevelChunk.setBlockState (minus OCEAN_FLOOR, which only the server would read).
    update_column(c.motion_blocking, c, min_y_, lx, y, lz, s, state::kMotionBlocking, state::kMotionBlocking);
    update_column(c.motion_blocking_no_leaves, c, min_y_, lx, y, lz, s, state::kMotionBlockingNoLeaves, state::kMotionBlockingNoLeaves);
    update_column(c.world_surface, c, min_y_, lx, y, lz, s, state::kIsAir, 0);
}

// ---------------------------------------------------------------------------- blocks

// LevelChunk.setBlockState + the sendBlockUpdated half of Level.setBlock
bool Dimension::set_block_raw(Chunk& c, i32 x, i32 y, i32 z, BlockState s) {
    const u32 si = static_cast<u32>((y - min_y_) >> 4);
    const u32 lx = static_cast<u32>(x & 15), ly = static_cast<u32>(y & 15), lz = static_cast<u32>(z & 15);
    Section& sec = c.sections[si];
    const bool was_empty = sec.only_air();
    if (was_empty && is_air(s)) return false;
    const BlockState old = sec.set(alloc_, Section::index(lx, ly, lz), s);
    if (old == s) return false;
    update_heightmaps(c, lx, y, lz, s);
    const bool empty = sec.only_air();
    if (was_empty != empty) light_.update_section_status(c, si, empty);
    if (light_differs(old, s)) {
        if (has_sky_) light_.update_sky_sources(c, lx, y, lz);
        light_.check_block(x, y, z);
    }
    c.modified = true;
    c.frame_valid = false;
    changes_.push_back({&c, static_cast<u16>(si), static_cast<u16>((lx << 8) | (lz << 4) | ly)});
    changes_sorted_ = false;
    return true;
}

bool Dimension::set_block(i32 x, i32 y, i32 z, BlockState s) {
    if (y < min_y_ || y > max_y_) return false;
    Chunk* c = map_.find(x >> 4, z >> 4);
    if (!c || !c->ticking) return false;
    if (!set_block_raw(*c, x, y, z, s)) return false;
    // BlockState.updateNeighbourShapes: of all updateShape() overrides, the only one a block this
    // server can hold has is SnowyBlock's - the block below re-evaluates `snowy` from what is now on
    // top of it. (Blocks with other overrides are not placeable yet; see mcgen's placement_kind.)
    if (y > min_y_) {
        const BlockState below = c->sections[static_cast<u32>((y - 1 - min_y_) >> 4)].get(
            static_cast<u32>(x & 15), static_cast<u32>((y - 1) & 15), static_cast<u32>(z & 15));
        const BlockInfo& info = block_info_of(below);
        if (info.flags & block_flag::kSnowyBelow) {
            const BlockState want = info.alt[(block_info_of(s).flags & block_flag::kSnowTag) ? 1 : 0];
            if (want != below) set_block_raw(*c, x, y - 1, z, want);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------- per tick

void Dimension::update_light() {
    if (light_.has_work()) light_.run_updates();
    for (Chunk* c : light_.changed()) c->frame_valid = false;
}

std::vector<Dimension::BlockChange>& Dimension::block_changes() {
    if (!changes_sorted_) {
        std::sort(changes_.begin(), changes_.end(), [](const BlockChange& a, const BlockChange& b) {
            if (a.chunk != b.chunk) return a.chunk->x != b.chunk->x ? a.chunk->x < b.chunk->x : a.chunk->z < b.chunk->z;
            return a.section != b.section ? a.section < b.section : a.pos < b.pos;
        });
        changes_.erase(std::unique(changes_.begin(), changes_.end()), changes_.end());
        changes_sorted_ = true;
    }
    return changes_;
}

void Dimension::end_tick() {
    changes_.clear();
    changes_sorted_ = true;
    for (Chunk* c : light_.changed()) {
        c->sky_changed = c->block_changed = 0;
        c->light_listed = false;
    }
    light_.changed().clear();
}

usize Dimension::memory_bytes() const {
    return alloc_.reserved_bytes() + chunk_pool_.reserved_bytes() + const_cast<LightEngine&>(light_).layer_pool().reserved_bytes() + frame_bytes_;
}

}  // namespace world
