// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/region.hpp"

#include <algorithm>
#include <cstring>

namespace worldgen {

namespace wg = proto::gen::worldgen;
namespace hm = world::heightmap;

namespace {

constexpr u32 kKept[] = {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves};

// Where in its chunk a block is, as a block entity and a scheduled tick are kept: (y - min_y) << 8 | z << 4 | x.
u32 place_of(i32 x, i32 y, i32 z) {
    return static_cast<u32>(y - settings::kMinY) << 8 | (static_cast<u32>(z) & 15) << 4 | (static_cast<u32>(x) & 15);
}

}  // namespace

Region::Region(RegionWork& work, const Overworld& world, world::GenChunk* const* chunks, i32 x0, i32 z0, u32 width, u32 depth)
    : work_(work), world_(world), x0_(x0), z0_(z0), width_(width), depth_(depth) {
    DCHECK(width * depth <= RegionWork::kChunks);
    for (u32 slot = 0; slot < width * depth; ++slot) {
        world::GenChunk* c = chunks[slot];
        DCHECK(c && c->x == x0 + static_cast<i32>(slot % width) && c->z == z0 + static_cast<i32>(slot / width));
        DCHECK(c->sections_count == Terrain::kSections);
        chunks_[slot] = c;
        dirty_[slot] = 0;
        for (u32 si = 0; si < Terrain::kSections; ++si) {
            flat_[slot * Terrain::kSections + si] = nullptr;
            non_air_[slot * Terrain::kSections + si] = c->sections[si].non_air;
        }
    }
    // The ticks the chunks came with: a second one for a block and type is not taken (ProtoChunkTicks).
    usize ticks = 0;
    for (u32 slot = 0; slot < width * depth; ++slot)
        ticks += chunks_[slot]->block_ticks.size() + chunks_[slot]->fluid_ticks.size();
    if (work_.tick_count != 0 || work_.ticks.empty()) {
        work_.ticks.assign(std::max<usize>(work_.ticks.size(), 1024), 0);
        work_.tick_count = 0;
    }
    if (ticks != 0)
        for (u32 slot = 0; slot < width * depth; ++slot) {
            for (const world::SavedTick& t : chunks_[slot]->block_ticks)
                tick_is_new(u64{slot} << 48 | u64{t.type} << 17 | u64{t.y} << 8 | u64{t.z} << 4 | t.x);
            for (const world::SavedTick& t : chunks_[slot]->fluid_ticks)
                tick_is_new(u64{1} << 63 | u64{slot} << 48 | u64{t.type} << 17 | u64{t.y} << 8 | u64{t.z} << 4 | t.x);
        }
}

void Region::begin(i32 chunk_x, i32 chunk_z) {
    DCHECK(chunk_x - 1 >= x0_ && chunk_x + 1 < x0_ + static_cast<i32>(width_));
    DCHECK(chunk_z - 1 >= z0_ && chunk_z + 1 < z0_ + static_cast<i32>(depth_));
    center_x_ = chunk_x;
    center_z_ = chunk_z;
    nine_ = static_cast<u32>(chunk_x - 1 - x0_) + static_cast<u32>(chunk_z - 1 - z0_) * width_;
    // ChunkPos.getWorldPosition: the chunk's corner, at y 0.
    random_ = world_.random(wg::random_name::kWorldgenRegionRandom).at(chunk_x * 16, 0, chunk_z * 16);
}

u16* Region::spread(u32 section) {
    DCHECK(used_ < RegionWork::kSections);
    u16* states = &work_.flat[usize{used_++} * 4096];
    const world::Section& packed = chunks_[section / Terrain::kSections]->sections[section % Terrain::kSections];
    if (packed.uniform())
        std::fill_n(states, 4096, packed.single);
    else
        packed.unpack(states);
    flat_[section] = states;
    return states;
}

void Region::commit() {
    for (u32 slot = 0; slot < width_ * depth_; ++slot) {
        world::GenChunk& c = *chunks_[slot];
        for (u32 bits = dirty_[slot]; bits; bits &= bits - 1) {
            const u32 si = static_cast<u32>(std::countr_zero(bits));
            c.sections[si].assign(c.alloc, flat_[slot * Terrain::kSections + si]);
        }
        dirty_[slot] = 0;
    }
    used_ = 0;
}

// Heightmap.update: a block that counts raises the column to above itself; one that does not,
// set where the column's top was, lets the column fall to the next that does.
void Region::update_height(world::GenChunk& chunk, u32 slot, u32 type, i32 x, i32 y, i32 z, u16 state) {
    i16& first = chunk.height[type][(static_cast<u32>(x) & 15) | (static_cast<u32>(z) & 15) << 4];
    const i32 up = y - kMinY;
    if (up <= first - 2) return;
    if (wg::kHeightmaps[state] >> type & 1) {
        if (up >= first) first = static_cast<i16>(up + 1);
    } else if (first - 1 == up) {
        (void)slot;
        for (i32 below = y - 1; below >= kMinY; --below) {
            if (wg::kHeightmaps[block(x, below, z)] >> type & 1) {
                first = static_cast<i16>(below + 1 - kMinY);
                return;
            }
        }
        first = 0;
    }
}

bool Region::set_block(i32 x, i32 y, i32 z, u16 state, u32 flags) {
    const u32 slot = slot_of(x >> 4, z >> 4);
    if (UNLIKELY(slot == kNoSlot)) {
        ++counts_.refused_writes;
        return false;
    }
    const u32 up = static_cast<u32>(y - kMinY);
    if (UNLIKELY(up >= static_cast<u32>(kHeight))) {
        // (ProtoChunk.setBlockState answers void air and sets nothing; the region goes on to leave a
        // block entity's placeholder beyond the world. Nothing of vanilla's does this.)
        ++counts_.outside_height;
        return true;
    }
    const u32 section = slot * Terrain::kSections + (up >> 4);
    // ProtoChunk.setBlockState: air into a section of nothing but air is not set at all.
    if (non_air_[section] == 0 && state == world::kAir) return true;
    u16* states = flat_[section];
    if (!states) states = spread(section);
    const u32 i = (up & 15) << 8 | (static_cast<u32>(z) & 15) << 4 | (static_cast<u32>(x) & 15);
    const u16 old = states[i];
    states[i] = state;
    dirty_[slot] |= 1u << (up >> 4);
    non_air_[section] = static_cast<u16>(non_air_[section] + !world::is_air(state) - !world::is_air(old));
    world::GenChunk& chunk = *chunks_[slot];
    for (const u32 type : kKept) update_height(chunk, slot, type, x, y, z, state);
    // WorldGenRegion.setBlock: a block that has a block entity gets a placeholder for it, whatever
    // was there; one that has none takes the block entity of the block it replaces away.
    if (UNLIKELY(world::flags_of(state) & world::state::kBlockEntity))
        put_placeholder(chunk, place_of(x, y, z));
    else if (UNLIKELY(world::flags_of(old) & world::state::kBlockEntity))
        drop_block_entity(chunk, place_of(x, y, z));
    // BlockState.getPostProcessPos: the block itself, or the one above it.
    if (const u32 marks = wg::kStateFacts[state] & (wg::kMarksItself | wg::kMarksAbove); UNLIKELY(marks) && !(flags & 16))
        mark_post_process(x, marks & wg::kMarksItself ? y : y + 1, z);
    return true;
}

bool Region::put_raw(i32 x, i32 y, i32 z, u16 state) {
    const u32 slot = slot_of(x >> 4, z >> 4);
    if (UNLIKELY(slot == kNoSlot)) {
        ++counts_.refused_writes;
        return false;
    }
    const u32 up = static_cast<u32>(y - kMinY);
    DCHECK(up < static_cast<u32>(kHeight));
    const u32 section = slot * Terrain::kSections + (up >> 4);
    u16* states = flat_[section];
    if (!states) states = spread(section);
    const u32 i = (up & 15) << 8 | (static_cast<u32>(z) & 15) << 4 | (static_cast<u32>(x) & 15);
    const u16 old = states[i];
    states[i] = state;
    dirty_[slot] |= 1u << (up >> 4);
    non_air_[section] = static_cast<u16>(non_air_[section] + !world::is_air(state) - !world::is_air(old));
    return true;
}

u16 Region::biome(i32 x, i32 y, i32 z) const {
    const QuartCell cell = zoom_cell(world_.zoom_seed(), x, y, z);
    const u32 slot = slot_of(cell.x >> 2, cell.z >> 2);
    // (LevelReader.getNoiseBiome asks the region for the chunk, and a region throws for one beyond the nine.)
    DCHECK(slot != kNoSlot);
    if (UNLIKELY(slot == kNoSlot)) return 0;
    // ChunkAccess.getNoiseBiome: y clamped into the world.
    const i32 qy = std::clamp(cell.y - (kMinY >> 2), 0, (kHeight >> 2) - 1);
    return chunks_[slot]
        ->cells[static_cast<u32>(qy >> 2) * 64 +
                (static_cast<u32>(qy & 3) << 4 | static_cast<u32>(cell.z & 3) << 2 | static_cast<u32>(cell.x & 3))];
}

void Region::possible_biomes(u64 out[2]) const {
    out[0] = out[1] = 0;
    for (u32 dz = 0; dz < 3; ++dz)
        for (u32 dx = 0; dx < 3; ++dx) {
            const world::GenChunk& c = *chunks_[nine_ + dx + dz * width_];
            out[0] |= c.biomes_present[0];
            out[1] |= c.biomes_present[1];
        }
}

void Region::mark_post_process(i32 x, i32 y, i32 z) {
    if (outside_height(y)) return;  // (ProtoChunk.markPosForPostProcessing)
    const u32 slot = slot_of(x >> 4, z >> 4);
    if (UNLIKELY(slot == kNoSlot)) {
        ++counts_.far_marks;
        return;
    }
    const u32 up = static_cast<u32>(y - kMinY);
    chunks_[slot]->marks.push_back((up >> 4) << 12 | (static_cast<u32>(x) & 15) | (up & 15) << 4 |
                                   (static_cast<u32>(z) & 15) << 8);
}

bool Region::tick_is_new(u64 key) {
    std::vector<u64>& table = work_.ticks;
    if (UNLIKELY((work_.tick_count + 1) * 2 > table.size())) {
        std::vector<u64> old(table.size() * 2, 0);
        old.swap(table);
        work_.tick_count = 0;
        for (const u64 k : old)
            if (k) tick_is_new(k - 1);
    }
    const usize mask = table.size() - 1;
    const u64 stored = key + 1;  // (0 is an empty place)
    for (usize at = (key * 0x9E3779B97F4A7C15ull) >> 20 & mask;; at = (at + 1) & mask) {
        if (table[at] == stored) return false;
        if (table[at] == 0) {
            table[at] = stored;
            ++work_.tick_count;
            return true;
        }
    }
}

void Region::schedule_tick(i32 x, i32 y, i32 z, u32 type, bool fluid, i32 priority) {
    const u32 slot = slot_of(x >> 4, z >> 4);
    if (UNLIKELY(slot == kNoSlot || outside_height(y))) {
        ++counts_.far_ticks;
        return;
    }
    const u32 up = static_cast<u32>(y - kMinY);
    const u8 lx = static_cast<u8>(x & 15), lz = static_cast<u8>(z & 15);
    if (!tick_is_new(u64{fluid} << 63 | u64{slot} << 48 | u64{type} << 17 | u64{up} << 8 | u64{lz} << 4 | lx)) return;
    world::GenChunk& chunk = *chunks_[slot];
    (fluid ? chunk.fluid_ticks : chunk.block_ticks).push_back({type, lx, lz, up, 0, static_cast<i8>(priority)});
}

const world::GenBlockEntity* Region::block_entity(i32 x, i32 y, i32 z) const {
    const u32 slot = slot_of(x >> 4, z >> 4);
    if (slot == kNoSlot || outside_height(y)) return nullptr;
    const u32 at = place_of(x, y, z);
    for (const world::GenBlockEntity& e : chunks_[slot]->block_entities)
        if (e.at == at) return &e;
    return nullptr;
}

std::span<const u8> Region::block_entity_tag(i32 x, i32 y, i32 z) const {
    const world::GenBlockEntity* e = block_entity(x, y, z);
    if (!e || e->len == 0) return {};
    return {chunks_[slot_of(x >> 4, z >> 4)]->tags.data() + e->tag, e->len};
}

bool Region::set_block_entity_tag(i32 x, i32 y, i32 z, std::span<const u8> tag) {
    const u32 slot = slot_of(x >> 4, z >> 4);
    if (slot == kNoSlot || outside_height(y)) return false;
    world::GenChunk& chunk = *chunks_[slot];
    const u32 at = place_of(x, y, z);
    for (world::GenBlockEntity& e : chunk.block_entities) {
        if (e.at != at) continue;
        // (A tag that is replaced stays where it lies until the chunk is finished: few are, ever.)
        e.tag = static_cast<u32>(chunk.tags.size());
        e.len = static_cast<u32>(tag.size());
        chunk.tags.insert(chunk.tags.end(), tag.begin(), tag.end());
        return true;
    }
    return false;
}

void Region::put_placeholder(world::GenChunk& chunk, u32 at) {
    for (world::GenBlockEntity& e : chunk.block_entities)
        if (e.at == at) {
            e.len = 0;  // ProtoChunk.setBlockEntityNbt: the placeholder takes the place of what was known
            return;
        }
    chunk.block_entities.push_back({at, 0, 0});
}

void Region::drop_block_entity(world::GenChunk& chunk, u32 at) {
    for (world::GenBlockEntity& e : chunk.block_entities)
        if (e.at == at) {
            e = chunk.block_entities.back();
            chunk.block_entities.pop_back();
            return;
        }
}

void prime_heightmaps(world::GenChunk& chunk, u32 types) {
    i32 top = 0;  // the first section from the top that is not all air, and one more
    for (i32 si = chunk.sections_count - 1; si >= 0 && top == 0; --si)
        if (!chunk.sections[si].only_air()) top = (si + 1) * 16;
    for (u32 i = 0; i < 256; ++i) {
        for (u32 type = 0; type < hm::kCount; ++type)
            if (types >> type & 1) chunk.height[type][i] = 0;
        u32 open = types;
        for (i32 up = top - 1; up >= 0 && open; --up) {
            const world::Section& section = chunk.sections[up >> 4];
            if (section.only_air()) {  // (ProtoChunk.getBlockState: such a section is air to whoever asks)
                up &= ~15;
                continue;
            }
            const u32 found = wg::kHeightmaps[section.get(i & 15, static_cast<u32>(up) & 15, i >> 4)] & open;
            if (!found) continue;
            for (u32 type = 0; type < hm::kCount; ++type)
                if (found >> type & 1) chunk.height[type][i] = static_cast<i16>(up + 1);
            open &= ~found;
        }
    }
}

}  // namespace worldgen
