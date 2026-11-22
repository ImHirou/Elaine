// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/dimension.hpp"

#include <cstring>

#include <algorithm>
#include <new>
#include <thread>

#include "base/clock.hpp"
#include "proto/frame.hpp"
#include "proto/varint.hpp"
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

// A frame on its way through a worker: the packet's body as the tick thread wrote it, and what
// the worker makes of it.
struct Dimension::FrameJob {
    Dimension* dim;
    i32 cx, cz;
    u64 version;
    i32 threshold;
    base::ByteBuf body, out;
};

Dimension::Dimension(u32 type_id, std::string name, ChunkGenerator& gen, i32 compression_threshold, base::Workers* workers,
                     std::string dir, bool sync)
    : type_id_(type_id),
      name_(std::move(name)),
      gen_(gen),
      threshold_(compression_threshold),
      min_y_(dim::kTypes[type_id].min_y),
      max_y_(dim::kTypes[type_id].min_y + dim::kTypes[type_id].height - 1),
      sections_(static_cast<u32>(dim::kTypes[type_id].height) / 16),
      has_sky_(dim::kTypes[type_id].has_skylight),
      layout_{min_y_, min_y_ >> 4, sections_, has_sky_},
      light_(map_, layout_),
      workers_(workers ? *workers : own_workers_),
      store_(workers_, layout_, gen_, std::move(dir), sync),
      ticks_(state::kBlockCount, min_y_, sections_ * 16) {}

Dimension::~Dimension() {
    store_.finish();  // what waits to be written is written; nothing of ours is out with a worker after this
    release_adopted();
    map_.for_each([this](Chunk& c) {
        ticks_.release(c);
        std::free(c.frame);
        free_block_entities(c);
        std::free(c.biomes);
        std::free(c.keep.bytes);
        light_.release_chunk(c);
    });
}

// ---------------------------------------------------------------------------- coming

// A chunk a worker made becomes a chunk of the world: its sections and light layers are copied out
// of the worker's arena into the pools here, and the light engine is told of it - with the layers
// it was saved with queued, to be taken up section by section (LightEngine::init_chunk).
Chunk& Dimension::adopt(LoadedChunk& lc) {
    const Chunk& from = lc.chunk;
    Chunk* c = new (chunk_pool_.alloc()) Chunk(from);
    for (u32 si = 0; si < sections_; ++si) {
        c->sections[si] = Section{};
        c->sections[si].copy_from(alloc_, from.sections[si]);
    }
    for (u32 li = 0; li < kMaxLightSections; ++li) c->sky[li].v = c->block[li].v = 0;
    for (u32 li = 0; li < sections_ + 2; ++li) {
        if (has_sky_) light_.queue_layer(*c, LightEngine::kSky, li, from.sky[li]);
        light_.queue_layer(*c, LightEngine::kBlock, li, from.block[li]);
    }
    if (from.biomes) {
        c->biomes = static_cast<BiomeSection*>(std::malloc(sizeof(BiomeSection) * kMaxSections));
        if (UNLIKELY(!c->biomes)) std::abort();
        std::memcpy(static_cast<void*>(c->biomes), from.biomes, sizeof(BiomeSection) * kMaxSections);
    }
    if (from.keep.len) {
        c->keep.bytes = static_cast<u8*>(std::malloc(from.keep.len));
        if (UNLIKELY(!c->keep.bytes)) std::abort();
        std::memcpy(c->keep.bytes, from.keep.bytes, from.keep.len);
    }
    c->status = ChunkStatus::Generated;
    c->frame_version = ++versions_;
    c->saved_tick = tick_;
    large_shapes_ |= (lc.traits & 1) != 0;
    entity_blocks_ |= (lc.traits & 2) != 0;
    c->sim_refs = c->block_refs = 0;  // whoever simulates says so again (Dimension::simulate)
    c->active_sections = 0;
    c->random_slot = Chunk::kUnlisted;
    c->block_ticks = nullptr;
    ticks_.read(*c, lc.parts.ticks);
    map_.set_loading(c->x, c->z, false);
    map_.insert(c);
    light_.init_chunk(*c);
    return *c;
}

// ---------------------------------------------------------------------------- ticking

// Which of a chunk's sections have a random tick that could do something: those with a cell
// that may always act, or one that is dying - or one that may spread, with soil to take within
// its reach. A spreader reaches one cell sideways and from three below to one above
// (SpreadingSnowyBlock.randomTick): not beyond the sections around its own.
u32 Dimension::active_mask(const Chunk& c) const {
    u32 targets = 0;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const Chunk* n = dx || dz ? map_.find(c.x + dx, c.z + dz) : &c;
            if (n) targets |= n->target_sections;
        }
    }
    const u32 near = targets | targets << 1 | targets >> 1;
    return c.ticking_sections & (c.always_sections | c.dying_sections | near);
}

void Dimension::refresh_active(Chunk& c) {
    const u32 active = active_mask(c);
    if (active == c.active_sections) return;
    c.active_sections = active;
    status_changed(c);
}

// Something that decides what ticks in the chunk changed: whether it ticks at all, who simulates
// it, which of its sections have random ticks to run.
void Dimension::status_changed(Chunk& c) {
    // ServerLevel.startTickingChunk: the ticks it was loaded with count from now.
    if (c.block_ticking() && c.block_ticks && c.block_ticks->saved_n) ticks_.unpack(c, static_cast<i64>(time_));
    const u32 sections = kCheckDormantSections ? c.ticking_sections : c.active_sections;
    const bool want = c.ticking && c.sim_refs != 0 && sections != 0;
    if (want == (c.random_slot != Chunk::kUnlisted)) return;
    if (want) {
        c.random_slot = static_cast<u32>(random_.size());
        random_.push_back(&c);
    } else {
        random_[c.random_slot] = random_.back();
        random_[c.random_slot]->random_slot = c.random_slot;
        random_.pop_back();
        c.random_slot = Chunk::kUnlisted;
    }
}

void Dimension::simulate(Chunk& c, i32 sim_delta, i32 block_delta) {
    c.sim_refs = static_cast<u16>(c.sim_refs + sim_delta);
    c.block_refs = static_cast<u16>(c.block_refs + block_delta);
    status_changed(c);
}

void Dimension::start_ticking(Chunk& c) {
    c.ticking = true;
    c.active_sections = active_mask(c);
    status_changed(c);
}

void Dimension::stop_ticking(Chunk& c) {
    c.ticking = false;
    status_changed(c);
}

// Around a chunk that has just come: what can be lit now is lit, what can tick now ticks.
void Dimension::promote_around(i32 cx, i32 cz) {
    const auto all_around = [&](i32 x, i32 z, ChunkStatus at_least) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const Chunk* n = map_.find(x + dx, z + dz);
                if (!n || n->status < at_least) return false;
            }
        }
        return true;
    };
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            Chunk* c = map_.find(cx + dx, cz + dz);
            if (c && c->status == ChunkStatus::LightReady && all_around(c->x, c->z, ChunkStatus::LightReady))
                light_.light_chunk(*c);
        }
    }
    for (i32 dz = -2; dz <= 2; ++dz) {
        for (i32 dx = -2; dx <= 2; ++dx) {
            Chunk* c = map_.find(cx + dx, cz + dz);
            if (!c || c->ticking || c->status != ChunkStatus::Lit || !all_around(c->x, c->z, ChunkStatus::Lit)) continue;
            start_ticking(*c);
            ready_.push_back(c);
        }
    }
}

void Dimension::pump(u64 tick, u64 deadline_ns) {
    tick_ = tick;
    workers_.poll();
    std::vector<LoadedChunk*>& loaded = store_.loaded();
    usize taken = 0;
    while (taken < loaded.size()) {
        LoadedChunk* lc = loaded[taken++];
        Chunk& c = adopt(*lc);
        if (keep_adopted_)
            adopted_.push_back({&c, lc});
        else
            delete lc;
        promote_around(c.x, c.z);
        if (deadline_ns && base::now_ns() >= deadline_ns) break;
    }
    loaded.erase(loaded.begin(), loaded.begin() + static_cast<std::ptrdiff_t>(taken));
    store_.tick(tick);
    workers_.kick();
}

void Dimension::release_adopted() {
    for (const Adopted& a : adopted_) delete a.from;
    adopted_.clear();
}

Chunk& Dimension::load(i32 cx, i32 cz) {
    if (Chunk* have = map_.find(cx, cz)) return *have;
    request(cx, cz);
    for (;;) {
        workers_.kick();
        workers_.poll();
        std::vector<LoadedChunk*>& loaded = store_.loaded();
        for (LoadedChunk* lc : loaded) {
            const bool mine = lc->chunk.x == cx && lc->chunk.z == cz;
            Chunk& c = adopt(*lc);
            if (keep_adopted_)
                adopted_.push_back({&c, lc});
            else
                delete lc;
            // One that was asked for through request() goes its usual way; the one asked for here
            // is the caller's to light.
            if (!mine) promote_around(c.x, c.z);
        }
        loaded.clear();
        if (Chunk* c = map_.find(cx, cz)) return *c;
        std::this_thread::yield();
    }
}

Chunk& Dimension::lit(i32 cx, i32 cz) {
    Chunk* c = map_.find(cx, cz);
    if (c && c->status == ChunkStatus::Lit) return *c;
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) load(cx + dx, cz + dz);
    c = map_.find(cx, cz);
    if (c->status != ChunkStatus::Lit) light_.light_chunk(*c);
    return *c;
}

Chunk& Dimension::ticking(i32 cx, i32 cz) {
    Chunk* c = map_.find(cx, cz);
    if (LIKELY(c && c->ticking)) return *c;
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) lit(cx + dx, cz + dz);
    c = map_.find(cx, cz);
    start_ticking(*c);
    return *c;
}

// ---------------------------------------------------------------------------- going

void Dimension::save(Chunk& c, const ChunkParts& parts) {
    light_.fold_layers(c);
    scratch_.clear();
    tick_scratch_.clear();
    LevelTicks::write(c.block_ticks, static_cast<i64>(time_), tick_scratch_);
    encode_chunk(c, {parts.block_entities, parts.entities, {tick_scratch_.data, tick_scratch_.len}}, scratch_);
    store_.save(c.x, c.z, {scratch_.data, scratch_.len});
    c.dirty = false;
    c.saved_tick = tick_;
}

void Dimension::unload(Chunk& c) {
    // Light can no longer cross into this chunk, so what is around it is no longer final: it does
    // not tick until the chunk is back.
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            Chunk* n = map_.find(c.x + dx, c.z + dz);
            if (!n || !n->ticking) continue;
            stop_ticking(*n);
            invalidate(*n);
            if (n != &c) stopped_.push_back(n);
        }
    }
    status_changed(c);  // (out of the list of chunks with random ticks, whether or not it ticked)
    DCHECK(c.random_slot == Chunk::kUnlisted);
    ticks_.release(c);
    std::erase(ready_, &c);
    std::erase(stopped_, &c);
    DCHECK(std::none_of(changes_.begin(), changes_.end(), [&](const BlockChange& b) { return b.chunk == &c; }));
    DCHECK(!c.light_listed);
    light_.unload_chunk(c);
    for (u32 si = 0; si < sections_; ++si) c.sections[si].release(alloc_);
    frame_bytes_ -= c.frame_len;
    std::free(c.frame);
    free_block_entities(c);
    std::free(c.biomes);
    std::free(c.keep.bytes);
    map_.remove(c.x, c.z);
    c.~Chunk();
    chunk_pool_.free(&c);
}

// ---------------------------------------------------------------------------- frames

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

// The packet's body - id and payload - for a worker to frame and deflate.
void Dimension::write_body(Chunk& c, base::ByteBuf& to) {
    for (u32 si = 0; si < sections_; ++si) c.sections[si].compact(alloc_);
    light_.fold_layers(c);
    u8* p = to.reserve(chunk_wire_bound(c) + proto::kVarIntMaxBytes);
    p = proto::put_varint(p, proto::ids::play::cb::kLevelChunkWithLight);
    p = put_chunk(p, c);
    to.commit(p);
}

std::span<const u8> Dimension::frame_if_ready(Chunk& c) {
    if (LIKELY(c.frame_valid)) return {c.frame, c.frame_len};
    if (c.frame_job == c.frame_version) return {};    // a worker is at it
    if (UNLIKELY(light_.has_work())) update_light();  // as in frame(); may make the version a newer one
    auto* job = new FrameJob{this, c.x, c.z, c.frame_version, threshold_, {}, {}};
    write_body(c, job->body);
    c.frame_job = c.frame_version;
    workers_.submit({&frame_work, &frame_done, this, job, 0, 0, 0, 0}, 1);
    return {};
}

void Dimension::frame_work(base::Task& t) {
    auto* job = static_cast<FrameJob*>(t.data);
    proto::frame_raw(job->out, job->threshold, {job->body.data, job->body.len});
}

void Dimension::frame_done(base::Task& t) {
    auto* job = static_cast<FrameJob*>(t.data);
    Dimension& d = *job->dim;
    // The chunk may have changed since, or gone, or gone and come back: the frame is of the
    // version it was made of, and versions are never used twice.
    Chunk* c = d.map_.find(job->cx, job->cz);
    if (c && c->frame_job == job->version) c->frame_job = 0;
    if (c && c->frame_version == job->version && !c->frame_valid) {
        d.frame_bytes_ += job->out.len;
        d.frame_bytes_ -= c->frame_len;
        std::free(c->frame);
        c->frame = job->out.data;  // the buffer's memory is the frame's from here on
        c->frame_len = job->out.len;
        c->frame_valid = true;
        job->out.data = nullptr;
        job->out.len = job->out.cap = 0;
    }
    delete job;
}

void Dimension::update_heightmaps(Chunk& c, u32 lx, i32 y, u32 lz, BlockState s) {
    // Same order as LevelChunk.setBlockState (minus OCEAN_FLOOR, which only the server would read).
    update_column(c.motion_blocking, c, min_y_, lx, y, lz, s, state::kMotionBlocking, state::kMotionBlocking);
    update_column(c.motion_blocking_no_leaves, c, min_y_, lx, y, lz, s, state::kMotionBlockingNoLeaves,
                  state::kMotionBlockingNoLeaves);
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
    if (UNLIKELY(state::kRandom[old] != state::kRandom[s])) recount_random(c, si, lx, y, lz, old, s);
    if (const u32 traits = section_traits(s); UNLIKELY(traits)) {
        large_shapes_ |= (traits & 1) != 0;
        entity_blocks_ |= (traits & 2) != 0;
        if (traits & 1) c.large_sections |= 1u << si;
        if (traits & 2) c.entity_sections |= 1u << si;
        if (traits & 4) c.fall_sections |= 1u << si;
    }
    update_heightmaps(c, lx, y, lz, s);
    const bool empty = sec.only_air();
    if (was_empty != empty) {
        light_.update_section_status(c, si, empty);
        c.filled_sections = empty ? c.filled_sections & ~(1u << si) : c.filled_sections | (1u << si);
    }
    if (light_differs(old, s)) {
        if (has_sky_) light_.update_sky_sources(c, lx, y, lz);
        light_.check_block(x, y, z);
    }
    c.modified = true;
    c.dirty = true;
    invalidate(c);
    changes_.push_back({&c, static_cast<u16>(si), static_cast<u16>((lx << 8) | (lz << 4) | ly)});
    changes_sorted_ = false;
    return true;
}

// What one changed cell does to the counts behind the random ticks (Chunk::random): its own
// part in them, under whatever stands on it, and the part of the cell below, on which it stands.
void Dimension::recount_random(Chunk& c, u32 si, u32 lx, i32 y, u32 lz, BlockState old, BlockState s) {
    constexpr u32 kUnder = state::kSpreader | state::kNylium | state::kSoil;
    constexpr u32 kOver = state::kSmothers | state::kNoSpread | state::kSmothersNylium;
    const u32 ro = state::kRandom[old], rs = state::kRandom[s];
    u32 changed = 0;  // 1: a mask of this chunk's own business; 2: where its soil is, which the chunks around read too
    const auto sync = [&](u32& mask, bool on, u32 bit, u32 kind) {
        if (((mask & bit) != 0) == on) return;
        mask ^= bit;
        changed |= kind;
    };
    const auto add = [](u16& count, bool now, bool before) { count = static_cast<u16>(count + now - before); };
    const u32 bit = 1u << si;
    RandomCounts& here = c.random[si];
    sync(c.ticking_sections, c.sections[si].ticking != 0, bit, 1);
    if ((ro ^ rs) & state::kRandomAlways) {
        add(here.always, (rs & state::kRandomAlways) != 0, (ro & state::kRandomAlways) != 0);
        sync(c.always_sections, here.always != 0, bit, 1);
    }
    if ((ro | rs) & kUnder) {
        const u32 above =
            y < max_y_
                ? state::kRandom[c.sections[static_cast<u32>((y + 1 - min_y_) >> 4)].get(lx, static_cast<u32>((y + 1) & 15), lz)]
                : 0u;
        add(here.dying, random_dies(rs, above), random_dies(ro, above));
        add(here.targets, random_target(rs, above), random_target(ro, above));
        sync(c.dying_sections, here.dying != 0, bit, 1);
        sync(c.target_sections, here.targets != 0, bit, 2);
    }
    if (((ro ^ rs) & kOver) && y > min_y_) {
        const u32 sb = static_cast<u32>((y - 1 - min_y_) >> 4);
        const u32 below = state::kRandom[c.sections[sb].get(lx, static_cast<u32>((y - 1) & 15), lz)];
        if (below & kUnder) {
            RandomCounts& under = c.random[sb];
            add(under.dying, random_dies(below, rs), random_dies(below, ro));
            add(under.targets, random_target(below, rs), random_target(below, ro));
            sync(c.dying_sections, under.dying != 0, 1u << sb, 1);
            sync(c.target_sections, under.targets != 0, 1u << sb, 2);
        }
    }
    if (changed & 2) {
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dx = -1; dx <= 1; ++dx)
                if (Chunk* n = map_.find(c.x + dx, c.z + dz)) refresh_active(*n);
    } else if (changed) {
        refresh_active(c);
    }
}

bool Dimension::set_block(i32 x, i32 y, i32 z, BlockState s) {
    if (y < min_y_ || y > max_y_) return false;
    Chunk* c = map_.find(x >> 4, z >> 4);
    if (!c || !c->ticking) return false;
    return set_block_raw(*c, x, y, z, s);
}

// ---------------------------------------------------------------------------- block entities

const BlockEntityRef* Dimension::block_entity(i32 x, i32 y, i32 z) const {
    const Chunk* c = map_.find(x >> 4, z >> 4);
    if (!c) return nullptr;
    const u8 xz = static_cast<u8>((x & 15) << 4 | (z & 15));
    for (u32 i = 0; i < c->block_entity_count; ++i)
        if (c->block_entities[i].xz == xz && c->block_entities[i].y == y) return &c->block_entities[i];
    return nullptr;
}

void Dimension::add_block_entity(i32 x, i32 y, i32 z, u32 type, u32 data) {
    Chunk* c = map_.find(x >> 4, z >> 4);
    DCHECK(c && !block_entity(x, y, z));
    if (c->block_entity_count == c->block_entity_cap) {  // amortized: a chunk's list only grows by placing blocks
        const u32 cap = c->block_entity_cap ? c->block_entity_cap * 2u : 4u;
        if (cap > 0xFFFF) std::abort();  // 65 536 block entities in one chunk: there are 98 304 blocks
        auto* grown = static_cast<BlockEntityRef*>(std::realloc(c->block_entities, cap * sizeof(BlockEntityRef)));
        if (UNLIKELY(!grown)) std::abort();
        c->block_entities = grown;
        if (c->block_entity_tags) {
            auto* tags = static_cast<BlockEntityTag*>(std::realloc(c->block_entity_tags, cap * sizeof(BlockEntityTag)));
            if (UNLIKELY(!tags)) std::abort();
            for (u32 i = c->block_entity_cap; i < cap; ++i) tags[i] = {};
            c->block_entity_tags = tags;
        }
        c->block_entity_cap = static_cast<u16>(cap);
    }
    if (c->block_entity_tags) c->block_entity_tags[c->block_entity_count] = {};
    c->block_entities[c->block_entity_count++] = {static_cast<u8>((x & 15) << 4 | (z & 15)), static_cast<u8>(type),
                                                  static_cast<i16>(y), data};
    c->dirty = true;
    invalidate(*c);
}

bool Dimension::remove_block_entity(i32 x, i32 y, i32 z, u32& data) {
    Chunk* c = map_.find(x >> 4, z >> 4);
    const BlockEntityRef* found = block_entity(x, y, z);
    if (!found) return false;
    data = found->data;
    const usize at = static_cast<usize>(found - c->block_entities);
    c->block_entities[at] = c->block_entities[--c->block_entity_count];
    if (c->block_entity_tags) {
        std::free(c->block_entity_tags[at].bytes);
        c->block_entity_tags[at] = c->block_entity_tags[c->block_entity_count];
        c->block_entity_tags[c->block_entity_count] = {};
    }
    c->dirty = true;
    invalidate(*c);
    return true;
}

void Dimension::free_block_entities(Chunk& c) {
    if (c.block_entity_tags)
        for (u32 i = 0; i < c.block_entity_count; ++i) std::free(c.block_entity_tags[i].bytes);
    std::free(c.block_entity_tags);
    std::free(c.block_entities);
    c.block_entity_tags = nullptr;
    c.block_entities = nullptr;
}

void Dimension::set_block_entity_tag(i32 x, i32 y, i32 z, std::span<const u8> tag) {
    Chunk* c = map_.find(x >> 4, z >> 4);
    const BlockEntityRef* found = block_entity(x, y, z);
    if (!found) return;
    if (!c->block_entity_tags) {
        if (tag.empty()) return;
        c->block_entity_tags = static_cast<BlockEntityTag*>(std::calloc(c->block_entity_cap, sizeof(BlockEntityTag)));
        if (UNLIKELY(!c->block_entity_tags)) std::abort();
    }
    BlockEntityTag& t = c->block_entity_tags[found - c->block_entities];
    std::free(t.bytes);
    t = {};
    if (!tag.empty()) {
        t.bytes = static_cast<u8*>(std::malloc(tag.size()));
        if (UNLIKELY(!t.bytes)) std::abort();
        std::memcpy(t.bytes, tag.data(), tag.size());
        t.len = static_cast<u32>(tag.size());
    }
    invalidate(*c);
}

std::span<const u8> Dimension::block_entity_tag(i32 x, i32 y, i32 z) const {
    const Chunk* c = map_.find(x >> 4, z >> 4);
    const BlockEntityRef* found = block_entity(x, y, z);
    if (!found || !c->block_entity_tags) return {};
    const BlockEntityTag& t = c->block_entity_tags[found - c->block_entities];
    return {t.bytes, t.len};
}

// ---------------------------------------------------------------------------- per tick

void Dimension::update_light() {
    if (light_.has_work()) light_.run_updates();
    for (Chunk* c : light_.changed()) invalidate(*c);
}

void Dimension::block_changed(i32 x, i32 y, i32 z) {
    Chunk* c = map_.find(x >> 4, z >> 4);
    if (!c || y < min_y() || y > max_y()) return;
    const u32 si = static_cast<u32>((y - min_y()) >> 4), lx = static_cast<u32>(x & 15), ly = static_cast<u32>(y & 15),
              lz = static_cast<u32>(z & 15);
    changes_.push_back({c, static_cast<u16>(si), static_cast<u16>((lx << 8) | (lz << 4) | ly)});
    changes_sorted_ = false;
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
    return alloc_.reserved_bytes() + chunk_pool_.reserved_bytes() +
           const_cast<LightEngine&>(light_).layer_pool().reserved_bytes() + frame_bytes_;
}

}  // namespace world
