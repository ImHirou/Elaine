// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/light.hpp"

#include <algorithm>

namespace world {

namespace {

// LightEngine.QueueEntry: level in the low 4 bits, one bit per direction to propagate in, two flags.
constexpr u32 kLevelMask = 15;
constexpr u32 kAllDirections = 0x3F0;
constexpr u32 kFromEmptyShape = 1024;  // the source block has no occluding shape: skip looking its state up
constexpr u32 kFromEmission = 2048;    // raise the source node itself to the level first
constexpr u32 dir_bit(u32 d) { return 1u << (d + 4); }
constexpr u32 with_level(u32 e, u32 level) { return (e & ~kLevelMask) | (level & kLevelMask); }
constexpr u32 decrease_all(u32 level) { return with_level(kAllDirections, level); }
constexpr u32 decrease_skip_one(u32 level, u32 skip) { return with_level(kAllDirections & ~dir_bit(skip), level); }
constexpr u32 increase_from_emission(u32 level, bool empty_shape) {
    return with_level(kAllDirections | kFromEmission | (empty_shape ? kFromEmptyShape : 0), level);
}
constexpr u32 increase_skip_one(u32 level, bool empty_shape, u32 skip) {
    return with_level((kAllDirections & ~dir_bit(skip)) | (empty_shape ? kFromEmptyShape : 0), level);
}
constexpr u32 increase_only_one(u32 level, bool empty_shape, u32 dir) {
    return with_level(dir_bit(dir) | (empty_shape ? kFromEmptyShape : 0), level);
}
constexpr u32 increase_sky_source(bool down, bool north, bool south, bool west, bool east) {
    return with_level((down ? dir_bit(kDown) : 0) | (north ? dir_bit(kNorth) : 0) | (south ? dir_bit(kSouth) : 0) |
                          (west ? dir_bit(kWest) : 0) | (east ? dir_bit(kEast) : 0),
                      15);
}
constexpr u32 kPullLightIn = decrease_all(1);  // LightEngine.PULL_LIGHT_IN_ENTRY
constexpr u32 kRemoveTopSkySource = decrease_all(15);
constexpr u32 kRemoveSkySource = decrease_skip_one(15, kUp);
constexpr u32 kAddSkySource = increase_skip_one(15, false, kUp);

// LightEngine.getState(): a chunk the engine cannot see is solid.
constexpr BlockState kUnloaded = proto::gen::block::kBedrock;

ALWAYS_INLINE bool edge_occluded(BlockState top, BlockState bottom) { return sky_edge_occluded(top, bottom); }

ALWAYS_INLINE bool crossed_chunk(i32 a, i32 b) { return ((a ^ b) & ~15) != 0; }

}  // namespace

void LightLayer::fold(BlockPool& pool) {
    if (!has_array()) return;
    const u8* a = array();
    const u8 b = a[0];
    if ((b >> 4) != (b & 15) || std::memcmp(a, a + 1, kBytes - 1) != 0) return;
    pool.free(array());
    v = 17 + (b & 15u);
}

LightEngine::LightEngine(ChunkMap& chunks, const Layout& l)
    : chunks_(chunks),
      min_y_(l.min_y),
      min_section_(l.min_section),
      min_light_section_(l.min_section - 1),
      sections_(l.sections),
      light_sections_(l.sections + 2),
      has_sky_(l.has_skylight),
      source_base_(l.min_y - 1),
      layout_(l) {
    DCHECK(l.sections <= kMaxSections);
}

// ---------------------------------------------------------------------------- storage helpers

BlockState LightEngine::state_at(i32 x, i32 y, i32 z) const {
    const Chunk* c = chunk_at(x >> 4, z >> 4);
    return c ? state_in(c, x, y, z) : kUnloaded;
}

bool LightEngine::storing(Kind k, i32 cx, i32 sy, i32 cz) const {
    Chunk* c = chunk_at(cx, cz);
    const u32 li = static_cast<u32>(sy - min_light_section_);
    return c && li < light_sections_ && layers(c, k)[li].present();
}

void LightEngine::list(Chunk* c) {
    // ChunkHolder.sectionLightChanged: nobody has been sent a chunk that is not ticking yet, and it has
    // no cached frame to invalidate either.
    if (c->ticking && !c->light_listed) {
        c->light_listed = true;
        changed_.push_back(c);
    }
}

void LightEngine::mark(Kind k, Chunk* c, u32 li) {
    if (li >= light_sections_ || !c->ticking) return;
    (k == kSky ? c->sky_changed : c->block_changed) |= 1u << li;
    list(c);
}

// SectionPos.aroundAndAtBlockPos: a changed light value concerns every section that touches the block.
void LightEngine::mark_block_and_around(Kind k, Chunk* c, i32 x, i32 y, i32 z) {
    const u32 lx = static_cast<u32>(x & 15), ly = static_cast<u32>(y & 15), lz = static_cast<u32>(z & 15);
    if (LIKELY(lx - 1 < 14 && ly - 1 < 14 && lz - 1 < 14)) {
        mark(k, c, light_index(y));
        return;
    }
    for (i32 sx = (x - 1) >> 4; sx <= (x + 1) >> 4; ++sx) {
        for (i32 sz = (z - 1) >> 4; sz <= (z + 1) >> 4; ++sz) {
            Chunk* n = (sx == c->x && sz == c->z) ? c : chunk_at(sx, sz);
            if (!n) continue;
            for (i32 sy = (y - 1) >> 4; sy <= (y + 1) >> 4; ++sy) mark(k, n, static_cast<u32>(sy - min_light_section_));
        }
    }
}

void LightEngine::mark_section_and_neighbours(Kind k, i32 cx, i32 sy, i32 cz) {
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            Chunk* n = chunk_at(cx + dx, cz + dz);
            if (!n) continue;
            for (i32 dy = -1; dy <= 1; ++dy) mark(k, n, static_cast<u32>(sy + dy - min_light_section_));
        }
    }
}

// LayerLightSectionStorage.setStoredLevel
ALWAYS_INLINE void LightEngine::set_level(Kind k, Chunk* c, u32 li, i32 x, i32 y, i32 z, u32 level) {
    layers(c, k)[li].set(pool_, nibble(x, y, z), level);
    c->dirty = true;  // ChunkHolder.sectionLightChanged: a chunk whose light changed is saved again
    mark_block_and_around(k, c, x, y, z);
}

// ---------------------------------------------------------------------------- section states

// LayerLightSectionStorage.updateSectionStatus
void LightEngine::update_section_status(Chunk& c, u32 section_index, bool empty, bool retain) {
    const u32 li = section_index + 1;  // light sections start one below the world
    const u8 state = c.light_state[li] & static_cast<u8>(~Chunk::kMarks);
    const u8 updated = empty ? static_cast<u8>(state & ~Chunk::kHasData) : static_cast<u8>(state | Chunk::kHasData);
    if (state == updated) return;
    put_section_state(c, li, updated, retain);
    const i32 inc = empty ? -1 : 1;
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                Chunk* n = (dx == 0 && dz == 0) ? &c : chunk_at(c.x + dx, c.z + dz);
                // A chunk that is not registered yet picks these counts up in init_chunk().
                if (!n || n->status < ChunkStatus::LightReady) continue;
                const u32 nli = li + static_cast<u32>(dy);
                if (nli >= light_sections_) continue;
                const u8 ns = n->light_state[nli] & static_cast<u8>(~Chunk::kMarks);
                const i32 count = (ns & Chunk::kNeighbourMask) + inc;
                DCHECK(count >= 0 && count <= 26);
                put_section_state(*n, nli, static_cast<u8>((ns & ~Chunk::kNeighbourMask) | count), retain);
            }
        }
    }
}

// LayerLightSectionStorage.putSectionState
void LightEngine::put_section_state(Chunk& c, u32 li, u8 state, bool retain) {
    u8& slot = c.light_state[li];
    const u8 marks = slot & Chunk::kMarks;
    const u8 old = slot & static_cast<u8>(~Chunk::kMarks);
    slot = state | marks;
    if (state != 0) {
        if (old == 0) initialize_section(c, li);
    } else if (old != 0) {  // removeSection: the layers go away at the end of the next run
        slot |= static_cast<u8>(Chunk::kPendingRemoval | (retain ? Chunk::kRetain : 0));
        if (!(marks & Chunk::kPendingRemoval)) removals_.push_back({&c, li});
        inconsistent_ = true;
    }
}

// LayerLightSectionStorage.initializeSection, for both layers
void LightEngine::initialize_section(Chunk& c, u32 li) {
    if (c.light_state[li] & Chunk::kPendingRemoval) {  // toRemove.remove(): it never lost its layers
        c.light_state[li] &= static_cast<u8>(~Chunk::kMarks);
        return;
    }
    const i32 sy = static_cast<i32>(li) + min_light_section_;
    // createDataLayer: the layer that was loaded for this section and has been waiting, if there is one.
    LightQueue* q = c.queued;
    if (q && q->block[li].present()) {
        c.block[li] = q->block[li];
        q->block[li].v = 0;
    } else {
        c.block[li].v = 1;  // new DataLayer()
    }
    mark_section_and_neighbours(kBlock, c.x, sy, c.z);
    if (has_sky_) {
        if (q && q->sky[li].present()) {
            c.sky[li] = q->sky[li];
            q->sky[li].v = 0;
        } else {
            c.sky[li] = create_sky_layer(c, li);
        }
        // SkyLightSectionStorage.onNodeAdded
        if (sky_lowest_ > sy) sky_lowest_ = sy;
        if (sky_top(c) < sy + 1) c.sky_top = sy + 1;
        mark_section_and_neighbours(kSky, c.x, sy, c.z);
    }
    inconsistent_ = true;
}

// SkyLightSectionStorage.createDataLayer
LightLayer LightEngine::create_sky_layer(Chunk& c, u32 li) {
    LightLayer out;
    const i32 sy = static_cast<i32>(li) + min_light_section_;
    if (sky_above_data(c, sy)) {
        out.v = c.sky_on ? 16 : 1;  // DataLayer(15) once the column is lit, an empty one before
        return out;
    }
    // Below the column's top: the section so far had the values of the bottom layer of the first
    // stored section above it. repeatFirstLayer() makes that explicit.
    u32 above = li + 1;
    while (above < light_sections_ && !c.sky[above].present()) ++above;
    if (above >= light_sections_) {  // cannot happen while sky_top is right; do not walk off the array if it is not
        DCHECK(false);
        out.v = 1;
        return out;
    }
    const LightLayer& src = c.sky[above];
    if (!src.has_array()) {
        out.v = src.v;
        return out;
    }
    u8* a = static_cast<u8*>(pool_.alloc());
    for (u32 y = 0; y < 16; ++y) std::memcpy(a + y * 128, src.array(), 128);
    out.v = reinterpret_cast<uintptr_t>(a);
    return out;
}

// LayerLightSectionStorage.markNewInconsistencies (the toRemove part) + SkyLightSectionStorage.onNodeRemoved
void LightEngine::process_removals(Kind) {
    for (const Pending& p : removals_) {
        Chunk& c = *p.c;
        if (!(c.light_state[p.li] & Chunk::kPendingRemoval)) continue;  // became needed again in the meantime
        // columnsToRetainQueuedDataFor: a layer that goes because the chunk beside it left the
        // world is kept, queued, for the day that chunk is back - vanilla has it on disk by then,
        // where a chunk this near the edge of what is loaded is not held long enough to matter.
        const bool retain = (c.light_state[p.li] & Chunk::kRetain) != 0;
        c.light_state[p.li] &= static_cast<u8>(~Chunk::kMarks);
        if (retain && (c.block[p.li].present() || c.sky[p.li].present())) {
            if (!c.queued) c.queued = new LightQueue();
            c.queued->block[p.li].drop(pool_);
            c.queued->sky[p.li].drop(pool_);
            c.queued->block[p.li] = c.block[p.li];
            c.queued->sky[p.li] = c.sky[p.li];
            c.block[p.li].v = c.sky[p.li].v = 0;
        } else if (c.queued) {  // queuedSections.remove(node)
            c.queued->block[p.li].drop(pool_);
            c.queued->sky[p.li].drop(pool_);
        }
        if (!retain) c.dirty = true;  // what it has on disk still names the layer
        c.block[p.li].drop(pool_);
        list(&c);  // the cached frame names this section; the client is not told (vanilla does not either)
        if (!has_sky_) continue;
        c.sky[p.li].drop(pool_);
        i32 y = static_cast<i32>(p.li) + min_light_section_;
        if (c.sky_top == y + 1) {
            while (!storing(kSky, c.x, y, c.z) && sky_data_at_or_below(y)) --y;
            c.sky_top = storing(kSky, c.x, y, c.z) ? y + 1 : Chunk::kNoSkyTop;
        }
    }
    removals_.clear();
}

// ---------------------------------------------------------------------------- chunk lifecycle

void LightEngine::init_chunk(Chunk& c) {
    DCHECK(c.status == ChunkStatus::Generated);
    c.status = ChunkStatus::LightReady;
    // Vanilla keeps section states in one map for the whole level, so a section of a chunk that
    // does not exist yet can already be "next to blocks". Here states live in chunks: collect what
    // the neighbours registered before this chunk came into being.
    u8 near[kMaxLightSections] = {};
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            if (dx == 0 && dz == 0) continue;
            const Chunk* n = chunk_at(c.x + dx, c.z + dz);
            if (!n || n->status < ChunkStatus::LightReady) continue;
            for (u32 li = 0; li < light_sections_; ++li) {
                if (!(n->light_state[li] & Chunk::kHasData)) continue;
                for (u32 t = li ? li - 1 : 0; t <= li + 1 && t < light_sections_; ++t) ++near[t];
            }
        }
    }
    for (u32 li = 0; li < light_sections_; ++li)
        if (near[li]) put_section_state(c, li, near[li]);
    // ThreadedLevelLightEngine.initializeLight
    for (u32 si = 0; si < sections_; ++si)
        if (!c.sections[si].only_air()) update_section_status(c, si, false);
    // ...and after the update: setLightEnabled(pos, lighted). A chunk that was saved lit has its
    // light on from here; what is created in its column later starts as it does in a lit one.
    if (c.loaded_lit) c.sky_on = c.block_on = true;
}

void LightEngine::light_chunk(Chunk& c) {
    DCHECK(c.status == ChunkStatus::LightReady);
    // "if (!lighted) propagateLightSources": a chunk that came lit from disk keeps the light it came with.
    if (!c.loaded_lit) {
        // LevelLightEngine.propagateLightSources: block engine first, then sky
        block_propagate_sources(c);
        if (has_sky_) sky_propagate_sources(c);
    }
    c.status = ChunkStatus::Lit;
    inconsistent_ = true;
    run_updates();
}

void LightEngine::release_chunk(Chunk& c) {
    for (u32 li = 0; li < kMaxLightSections; ++li) {
        c.sky[li].drop(pool_);
        c.block[li].drop(pool_);
        if (c.queued) {
            c.queued->sky[li].drop(pool_);
            c.queued->block[li].drop(pool_);
        }
    }
    delete c.queued;
    c.queued = nullptr;
}

// ThreadedLevelLightEngine.updateChunkStatus: the chunk leaves the world. Its light is off, its
// sections count as empty - the sections of the chunks around it that stored light only because
// of its blocks stop doing so (their layers are kept, see process_removals) - and its own layers go.
void LightEngine::unload_chunk(Chunk& c) {
    c.sky_on = c.block_on = false;
    for (u32 si = 0; si < sections_; ++si)
        if (c.light_state[si + 1] & Chunk::kHasData) update_section_status(c, si, true, true);
    std::erase_if(removals_, [&](const Pending& p) { return p.c == &c; });
    release_chunk(c);
    c.status = ChunkStatus::Generated;
}

// A layer that was loaded for light section `li` of a chunk that is not registered yet
// (LevelLightEngine.queueSectionData): the array is copied into the engine's pool.
void LightEngine::queue_layer(Chunk& c, Kind k, u32 li, const LightLayer& loaded) {
    if (!loaded.present()) return;
    if (!c.queued) c.queued = new LightQueue();
    LightLayer& to = (k == kSky ? c.queued->sky : c.queued->block)[li];
    to.drop(pool_);
    if (loaded.has_array()) {
        u8* a = static_cast<u8*>(pool_.alloc());
        std::memcpy(a, loaded.array(), LightLayer::kBytes);
        to.v = reinterpret_cast<uintptr_t>(a);
    } else {
        to.v = loaded.v;
    }
}

void LightEngine::fold_layers(Chunk& c) {
    for (u32 li = 0; li < light_sections_; ++li) {
        c.sky[li].fold(pool_);
        c.block[li].fold(pool_);
    }
}

// ---------------------------------------------------------------------------- driver

void LightEngine::check_block(i32 x, i32 y, i32 z) { check_.push_back({x, y, z, 0}); }

// LevelLightEngine.runLightUpdates
u32 LightEngine::run_updates() {
    if (check_.size() > 1) {  // vanilla keeps a set; the order of a set is not part of the algorithm
        std::sort(check_.begin(), check_.end(), [](const Node& a, const Node& b) {
            return a.x != b.x ? a.x < b.x : a.z != b.z ? a.z < b.z : a.y < b.y;
        });
        check_.erase(std::unique(check_.begin(), check_.end(),
                                 [](const Node& a, const Node& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }),
                     check_.end());
    }
    u32 n = run(kBlock);
    if (has_sky_) n += run(kSky);
    check_.clear();
    process_removals(kBlock);
    inconsistent_ = false;
    return n;
}

// LightEngine.runLightUpdates for one layer
u32 LightEngine::run(Kind k) {
    for (const Node& n : check_) k == kSky ? sky_check_node(n) : block_check_node(n);
    u32 count = 0;
    Fifo& dec = dec_[k];
    Fifo& inc = inc_[k];
    while (!dec.empty()) {  // propagateDecreases
        const Node n = dec.pop();
        k == kSky ? sky_propagate_decrease(n) : block_propagate_decrease(n);
        ++count;
    }
    while (!inc.empty()) {  // propagateIncreases
        const Node n = inc.pop();
        ++count;
        Chunk* c = chunk_at(n.x >> 4, n.z >> 4);
        const u32 li = light_index(n.y);
        if (UNLIKELY(!c || li >= light_sections_ || !layers(c, k)[li].present())) continue;
        u32 from = layers(c, k)[li].get(nibble(n.x, n.y, n.z));
        const u32 target = n.data & kLevelMask;
        if ((n.data & kFromEmission) && from < target) {
            set_level(k, c, li, n.x, n.y, n.z, target);
            from = target;
        }
        if (from == target) k == kSky ? sky_propagate_increase(n, from) : block_propagate_increase(n, from);
    }
    return count;
}

// ---------------------------------------------------------------------------- block light

// BlockLightEngine.checkNode
void LightEngine::block_check_node(const Node& n) {
    Chunk* c = chunk_at(n.x >> 4, n.z >> 4);
    const u32 li = light_index(n.y);
    if (!c || li >= light_sections_ || !c->block[li].present()) return;
    const BlockState s = state_in(c, n.x, n.y, n.z);
    const u32 em = c->block_on ? emission(s) : 0;  // getEmission(): only once the column's light is on
    const u32 old = c->block[li].get(nibble(n.x, n.y, n.z));
    if (em < old) {
        set_level(kBlock, c, li, n.x, n.y, n.z, 0);
        dec_[kBlock].push(n.x, n.y, n.z, decrease_all(old));
    } else {
        dec_[kBlock].push(n.x, n.y, n.z, kPullLightIn);
    }
    if (em > 0) inc_[kBlock].push(n.x, n.y, n.z, increase_from_emission(em, has_empty_shape(s)));
}

// BlockLightEngine.propagateIncrease
void LightEngine::block_propagate_increase(const Node& n, u32 from_level) {
    Chunk* fc = chunk_at(n.x >> 4, n.z >> 4);
    BlockState from_state = kAir;
    bool have_from = (n.data & kFromEmptyShape) != 0;  // then the source counts as air for occlusion
    for (u32 d = 0; d < 6; ++d) {
        if (!(n.data & dir_bit(d))) continue;
        const i32 x = n.x + kStepX[d], y = n.y + kStepY[d], z = n.z + kStepZ[d];
        Chunk* c = (crossed_chunk(x, n.x) || crossed_chunk(z, n.z)) ? chunk_at(x >> 4, z >> 4) : fc;
        const u32 li = light_index(y);
        if (!c || li >= light_sections_) continue;
        LightLayer& layer = c->block[li];
        if (!layer.present()) continue;
        const u32 to_level = layer.get(nibble(x, y, z));
        if (static_cast<i32>(from_level) - 1 <= static_cast<i32>(to_level)) continue;
        const BlockState to_state = state_in(c, x, y, z);
        const i32 new_level = static_cast<i32>(from_level) - static_cast<i32>(opacity(to_state));
        if (new_level <= static_cast<i32>(to_level)) continue;
        if (!have_from) {
            from_state = fc ? state_in(fc, n.x, n.y, n.z) : kUnloaded;
            have_from = true;
        }
        if (shape_occludes(from_state, to_state, d)) continue;
        set_level(kBlock, c, li, x, y, z, static_cast<u32>(new_level));
        if (new_level > 1)
            inc_[kBlock].push(x, y, z, increase_skip_one(static_cast<u32>(new_level), has_empty_shape(to_state), opposite(d)));
    }
}

// BlockLightEngine.propagateDecrease
void LightEngine::block_propagate_decrease(const Node& n) {
    Chunk* fc = chunk_at(n.x >> 4, n.z >> 4);
    const u32 old_from = n.data & kLevelMask;
    for (u32 d = 0; d < 6; ++d) {
        if (!(n.data & dir_bit(d))) continue;
        const i32 x = n.x + kStepX[d], y = n.y + kStepY[d], z = n.z + kStepZ[d];
        Chunk* c = (crossed_chunk(x, n.x) || crossed_chunk(z, n.z)) ? chunk_at(x >> 4, z >> 4) : fc;
        const u32 li = light_index(y);
        if (!c || li >= light_sections_) continue;
        LightLayer& layer = c->block[li];
        if (!layer.present()) continue;
        const u32 to_level = layer.get(nibble(x, y, z));
        if (to_level == 0) continue;
        if (static_cast<i32>(to_level) <= static_cast<i32>(old_from) - 1) {
            const BlockState to_state = state_in(c, x, y, z);
            const u32 em = c->block_on ? emission(to_state) : 0;
            set_level(kBlock, c, li, x, y, z, 0);
            if (em < to_level) dec_[kBlock].push(x, y, z, decrease_skip_one(to_level, opposite(d)));
            if (em > 0) inc_[kBlock].push(x, y, z, increase_from_emission(em, has_empty_shape(to_state)));
        } else {
            inc_[kBlock].push(x, y, z, increase_only_one(to_level, false, opposite(d)));
        }
    }
}

// BlockLightEngine.propagateLightSources
void LightEngine::block_propagate_sources(Chunk& c) {
    c.block_on = true;
    for (u32 si = 0; si < sections_; ++si) {
        const Section& s = c.sections[si];
        if (s.only_air()) continue;
        bool any = false;
        if (s.bits == 0) {
            any = emission(s.single) != 0;
        } else if (s.bits == Section::kDirectBits) {
            any = true;
        } else {
            const u16* refs = s.refs();
            for (u32 i = 0; i < s.palette_len; ++i) any |= refs[i] && emission(s.palette[i]);
        }
        if (!any) continue;
        BlockState states[Section::kVolume];
        s.unpack(states);
        const i32 bx = c.x * 16, by = (min_section_ + static_cast<i32>(si)) * 16, bz = c.z * 16;
        for (u32 i = 0; i < Section::kVolume; ++i) {
            const u32 em = emission(states[i]);
            if (!em) continue;
            inc_[kBlock].push(bx + static_cast<i32>(i & 15), by + static_cast<i32>(i >> 8), bz + static_cast<i32>((i >> 4) & 15),
                              increase_from_emission(em, has_empty_shape(states[i])));
        }
    }
}

// ---------------------------------------------------------------------------- sky light sources

i32 LightEngine::lowest_source_y(const Chunk& c, u32 lx, u32 lz) const {
    const i32 v = raw_source(c, lx + lz * 16);
    return v == source_base_ ? INT32_MIN : v;  // extendSourcesBelowWorld
}

// SkyLightEngine.getLowestSourceY
i32 LightEngine::lowest_source_at(i32 x, i32 z, i32 fallback) const {
    const Chunk* c = chunk_at(x >> 4, z >> 4);
    return c ? lowest_source_y(*c, static_cast<u32>(x & 15), static_cast<u32>(z & 15)) : fallback;
}

// ChunkSkyLightSources.fillFrom
void LightEngine::fill_sky_sources(Chunk& c) { world::fill_sky_sources(c, layout_); }

// ChunkSkyLightSources.findLowestSourceBelow
i32 LightEngine::find_lowest_source_below(const Chunk& c, u32 lx, i32 start_y, u32 lz, BlockState start_state) const {
    i32 top_y = start_y;
    BlockState top = start_state;
    for (i32 y = start_y - 1; y >= source_base_; --y) {
        const BlockState bottom = state_in(&c, static_cast<i32>(lx), y, static_cast<i32>(lz));
        if (edge_occluded(top, bottom)) return top_y;
        top = bottom;
        top_y = y;
    }
    return source_base_;
}

// ChunkSkyLightSources.updateEdge
bool LightEngine::update_source_edge(Chunk& c, u32 i, i32 old_edge, u32 lx, u32 lz, i32 top_y, BlockState top,
                                     BlockState bottom) {
    if (edge_occluded(top, bottom)) {
        if (top_y > old_edge) {
            c.sky_sources.set(i, static_cast<u32>(top_y - source_base_));
            return true;
        }
    } else if (top_y == old_edge) {
        c.sky_sources.set(i, static_cast<u32>(find_lowest_source_below(c, lx, top_y - 1, lz, bottom) - source_base_));
        return true;
    }
    return false;
}

// ChunkSkyLightSources.update
bool LightEngine::update_sky_sources(Chunk& c, u32 lx, i32 y, u32 lz) {
    const u32 i = lx + lz * 16;
    const i32 current = raw_source(c, i);
    if (y + 1 < current) return false;
    const i32 x = static_cast<i32>(lx), z = static_cast<i32>(lz);
    const BlockState top = state_in(&c, x, y + 1, z);
    const BlockState middle = state_in(&c, x, y, z);
    if (update_source_edge(c, i, current, lx, lz, y + 1, top, middle)) return true;
    const BlockState bottom = state_in(&c, x, y - 1, z);
    return update_source_edge(c, i, current, lx, lz, y, middle, bottom);
}

// ---------------------------------------------------------------------------- sky light

// SkyLightEngine.checkNode
void LightEngine::sky_check_node(const Node& n) {
    Chunk* c = chunk_at(n.x >> 4, n.z >> 4);
    const i32 lowest = (c && c->sky_on) ? lowest_source_y(*c, static_cast<u32>(n.x & 15), static_cast<u32>(n.z & 15)) : INT32_MAX;
    if (lowest != INT32_MAX) sky_update_sources_in_column(n.x, n.z, lowest);
    const u32 li = light_index(n.y);
    if (!c || li >= light_sections_ || !c->sky[li].present()) return;
    if (n.y >= lowest) {
        dec_[kSky].push(n.x, n.y, n.z, kRemoveSkySource);
        inc_[kSky].push(n.x, n.y, n.z, kAddSkySource);
    } else {
        const u32 old = c->sky[li].get(nibble(n.x, n.y, n.z));
        if (old > 0) {
            set_level(kSky, c, li, n.x, n.y, n.z, 0);
            dec_[kSky].push(n.x, n.y, n.z, decrease_all(old));
        } else {
            dec_[kSky].push(n.x, n.y, n.z, kPullLightIn);
        }
    }
}

// SkyLightEngine.updateSourcesInColumn
void LightEngine::sky_update_sources_in_column(i32 x, i32 z, i32 lowest) {
    const i32 world_bottom = sky_lowest_ == INT32_MAX ? INT32_MAX : sky_lowest_ * 16;
    sky_remove_sources_below(x, z, lowest, world_bottom);
    sky_add_sources_above(x, z, lowest, world_bottom);
}

// SkyLightEngine.removeSourcesBelow: the column's sources now start higher - what used to be direct
// sky light below that point has to be re-derived.
void LightEngine::sky_remove_sources_below(i32 x, i32 z, i32 lowest, i32 world_bottom) {
    if (lowest <= world_bottom) return;
    Chunk* c = chunk_at(x >> 4, z >> 4);
    if (!c) return;
    const i32 start = lowest - 1;
    for (i32 sy = start >> 4; sky_data_at_or_below(sy); --sy) {
        const u32 li = static_cast<u32>(sy - min_light_section_);
        if (li >= light_sections_ || !c->sky[li].present()) continue;
        const i32 bottom = sy * 16;
        for (i32 y = std::min(bottom + 15, start); y >= bottom; --y) {
            if (c->sky[li].get(nibble(x, y, z)) != 15) return;
            set_level(kSky, c, li, x, y, z, 0);
            dec_[kSky].push(x, y, z, y == lowest - 1 ? kRemoveTopSkySource : kRemoveSkySource);
        }
    }
}

// SkyLightEngine.addSourcesAbove: the column's sources now start lower - fill the newly open part.
void LightEngine::sky_add_sources_above(i32 x, i32 z, i32 lowest, i32 world_bottom) {
    Chunk* c = chunk_at(x >> 4, z >> 4);
    if (!c) return;
    const i32 neighbour_lowest = std::max(std::max(lowest_source_at(x - 1, z, INT32_MIN), lowest_source_at(x + 1, z, INT32_MIN)),
                                          std::max(lowest_source_at(x, z - 1, INT32_MIN), lowest_source_at(x, z + 1, INT32_MIN)));
    const i32 start = std::max(lowest, world_bottom);
    for (i32 sy = start >> 4; !sky_above_data(*c, sy); ++sy) {
        const u32 li = static_cast<u32>(sy - min_light_section_);
        if (li >= light_sections_ || !c->sky[li].present()) continue;
        const i32 bottom = sy * 16;
        for (i32 y = std::max(bottom, start); y <= bottom + 15; ++y) {
            if (c->sky[li].get(nibble(x, y, z)) == 15) return;
            set_level(kSky, c, li, x, y, z, 15);
            if (y < neighbour_lowest || y == lowest) inc_[kSky].push(x, y, z, kAddSkySource);
        }
    }
}

// SkyLightEngine.countEmptySectionsBelowIfAtBorder
u32 LightEngine::sky_empty_sections_below(const Node& n) const {
    if ((n.y & 15) != 0) return 0;
    const i32 lx = n.x & 15, lz = n.z & 15;
    if (lx != 0 && lx != 15 && lz != 0 && lz != 15) return 0;
    const i32 cx = n.x >> 4, sy = n.y >> 4, cz = n.z >> 4;
    u32 empty = 0;
    while (!storing(kSky, cx, sy - static_cast<i32>(empty) - 1, cz) && sky_data_at_or_below(sy - static_cast<i32>(empty) - 1))
        ++empty;
    return empty;
}

// SkyLightEngine.propagateFromEmptySections: sections without storage below `from` hold the value of
// its bottom layer, so light that just crossed into the neighbouring column next to `from` also
// reaches the stored sections beside those empty ones.
void LightEngine::sky_propagate_from_empty_sections(i32 x, i32 y, i32 z, u32 dir, u32 level, bool increase, u32 empty_below) {
    if (empty_below == 0) return;
    const i32 lx = x & 15, lz = z & 15;
    const bool crossed = dir == kNorth   ? lz == 15
                         : dir == kSouth ? lz == 0
                         : dir == kWest  ? lx == 15
                         : dir == kEast  ? lx == 0
                                         : false;
    if (!crossed) return;
    Chunk* c = chunk_at(x >> 4, z >> 4);
    if (!c) return;
    const i32 first = (y >> 4) - 1;
    for (i32 sy = first; sy > first - static_cast<i32>(empty_below); --sy) {
        const u32 li = static_cast<u32>(sy - min_light_section_);
        if (li >= light_sections_ || !c->sky[li].present()) continue;
        for (i32 ly = 15; ly >= 0; --ly) {
            const i32 by = sy * 16 + ly;
            if (increase) {
                set_level(kSky, c, li, x, by, z, level);
                if (level > 1) inc_[kSky].push(x, by, z, increase_skip_one(level, true, opposite(dir)));
            } else {
                set_level(kSky, c, li, x, by, z, 0);
                dec_[kSky].push(x, by, z, decrease_skip_one(level, opposite(dir)));
            }
        }
    }
}

// SkyLightEngine.propagateIncrease
void LightEngine::sky_propagate_increase(const Node& n, u32 from_level) {
    Chunk* fc = chunk_at(n.x >> 4, n.z >> 4);
    BlockState from_state = kAir;
    bool have_from = (n.data & kFromEmptyShape) != 0;
    const u32 empty_below = sky_empty_sections_below(n);
    for (u32 d = 0; d < 6; ++d) {
        if (!(n.data & dir_bit(d))) continue;
        const i32 x = n.x + kStepX[d], y = n.y + kStepY[d], z = n.z + kStepZ[d];
        Chunk* c = (crossed_chunk(x, n.x) || crossed_chunk(z, n.z)) ? chunk_at(x >> 4, z >> 4) : fc;
        const u32 li = light_index(y);
        if (!c || li >= light_sections_) continue;
        LightLayer& layer = c->sky[li];
        if (!layer.present()) continue;
        const u32 to_level = layer.get(nibble(x, y, z));
        if (static_cast<i32>(from_level) - 1 <= static_cast<i32>(to_level)) continue;
        const BlockState to_state = state_in(c, x, y, z);
        const i32 new_level = static_cast<i32>(from_level) - static_cast<i32>(opacity(to_state));
        if (new_level <= static_cast<i32>(to_level)) continue;
        if (!have_from) {
            from_state = fc ? state_in(fc, n.x, n.y, n.z) : kUnloaded;
            have_from = true;
        }
        if (shape_occludes(from_state, to_state, d)) continue;
        set_level(kSky, c, li, x, y, z, static_cast<u32>(new_level));
        if (new_level > 1)
            inc_[kSky].push(x, y, z, increase_skip_one(static_cast<u32>(new_level), has_empty_shape(to_state), opposite(d)));
        sky_propagate_from_empty_sections(x, y, z, d, static_cast<u32>(new_level), true, empty_below);
    }
}

// SkyLightEngine.propagateDecrease
void LightEngine::sky_propagate_decrease(const Node& n) {
    Chunk* fc = chunk_at(n.x >> 4, n.z >> 4);
    const u32 empty_below = sky_empty_sections_below(n);
    const u32 old_from = n.data & kLevelMask;
    for (u32 d = 0; d < 6; ++d) {
        if (!(n.data & dir_bit(d))) continue;
        const i32 x = n.x + kStepX[d], y = n.y + kStepY[d], z = n.z + kStepZ[d];
        Chunk* c = (crossed_chunk(x, n.x) || crossed_chunk(z, n.z)) ? chunk_at(x >> 4, z >> 4) : fc;
        const u32 li = light_index(y);
        if (!c || li >= light_sections_) continue;
        LightLayer& layer = c->sky[li];
        if (!layer.present()) continue;
        const u32 to_level = layer.get(nibble(x, y, z));
        if (to_level == 0) continue;
        if (static_cast<i32>(to_level) <= static_cast<i32>(old_from) - 1) {
            set_level(kSky, c, li, x, y, z, 0);
            dec_[kSky].push(x, y, z, decrease_skip_one(to_level, opposite(d)));
            sky_propagate_from_empty_sections(x, y, z, d, to_level, false, empty_below);
        } else {
            inc_[kSky].push(x, y, z, increase_only_one(to_level, false, opposite(d)));
        }
    }
}

// SkyLightEngine.propagateLightSources: first lighting of a column. Everything at or above a
// column's lowest source is 15; only cells that can shine into something darker are queued.
void LightEngine::sky_propagate_sources(Chunk& c) {
    c.sky_on = true;
    const Chunk* north = chunk_at(c.x, c.z - 1);
    const Chunk* south = chunk_at(c.x, c.z + 1);
    const Chunk* west = chunk_at(c.x - 1, c.z);
    const Chunk* east = chunk_at(c.x + 1, c.z);
    // A missing neighbour counts as open sky all the way down (emptyChunkSources).
    const auto src = [this](const Chunk* n, u32 x, u32 z) { return n ? lowest_source_y(*n, x, z) : INT32_MIN; };
    if (sky_lowest_ == INT32_MAX) return;
    const i32 top = sky_top(c);
    for (i32 sy = top - 1; sy >= sky_lowest_; --sy) {
        const u32 li = static_cast<u32>(sy - min_light_section_);
        if (li >= light_sections_ || !c.sky[li].present()) continue;
        const i32 section_min = sy * 16, section_max = section_min + 15;
        bool sources_below = false;
        for (u32 z = 0; z < 16; ++z) {
            for (u32 x = 0; x < 16; ++x) {
                const i32 lowest = lowest_source_y(c, x, z);
                if (lowest > section_max) continue;
                const i32 n_low = z == 0 ? src(north, x, 15) : lowest_source_y(c, x, z - 1);
                const i32 s_low = z == 15 ? src(south, x, 0) : lowest_source_y(c, x, z + 1);
                const i32 w_low = x == 0 ? src(west, 15, z) : lowest_source_y(c, x - 1, z);
                const i32 e_low = x == 15 ? src(east, 0, z) : lowest_source_y(c, x + 1, z);
                const i32 neighbour_lowest = std::max(std::max(n_low, s_low), std::max(w_low, e_low));
                for (i32 y = section_max; y >= std::max(section_min, lowest); --y) {
                    c.sky[li].set(pool_, (static_cast<u32>(y & 15) << 8) | (z << 4) | x, 15);
                    if (y == lowest || y < neighbour_lowest)
                        inc_[kSky].push(c.x * 16 + static_cast<i32>(x), y, c.z * 16 + static_cast<i32>(z),
                                        increase_sky_source(y == lowest, y < n_low, y < s_low, y < w_low, y < e_low));
                }
                if (lowest < section_min) sources_below = true;
            }
        }
        if (!sources_below) break;
    }
}

// ---------------------------------------------------------------------------- queries

u32 LightEngine::light(Kind k, i32 x, i32 y, i32 z) const {
    const Chunk* c = chunk_at(x >> 4, z >> 4);
    const u32 li = light_index(y);
    if (k == kBlock) {
        // BlockLightSectionStorage.getLightValue
        if (!c || li >= light_sections_ || !c->block[li].present()) return 0;
        return c->block[li].get(nibble(x, y, z));
    }
    if (!has_sky_) return 0;
    // SkyLightSectionStorage.getLightValue
    if (!c) return 15;
    const i32 top = sky_top(*c), sy = y >> 4;
    if (top == sky_lowest_ || sy >= top) return 15;
    if (li < light_sections_ && c->sky[li].present()) return c->sky[li].get(nibble(x, y, z));
    for (i32 s = sy + 1; s < top; ++s) {  // no storage here: the bottom layer of the first stored section above
        const u32 ai = static_cast<u32>(s - min_light_section_);
        if (ai < light_sections_ && c->sky[ai].present()) return c->sky[ai].get(nibble(x, 0, z));
    }
    return 15;
}

}  // namespace world
