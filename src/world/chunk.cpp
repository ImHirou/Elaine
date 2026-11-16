#include "world/chunk.hpp"

#include PROTO_GEN_PHYSICS_HPP

namespace world {

namespace {

namespace physics = proto::gen::physics;

// What collision queries and entity physics want to know of a section before they look at its
// blocks one by one: bit 0, a state whose collision shape reaches out of its block
// (BlockState.hasLargeCollisionShape); bit 1, one that acts on an entity in it or on it; bit 2, one
// that breaks a fall (#fall_damage_resetting).
u32 traits_of_section(const Section& s) {
    if (s.bits == 0) return section_traits(s.single);
    u32 t = 0;
    if (s.bits == Section::kDirectBits) {
        for (u32 i = 0; i < Section::kVolume; ++i) t |= section_traits(s.get(i));
        return t;
    }
    for (u32 i = 0; i < s.palette_len; ++i)
        if (s.palette[i] != Section::kNoState) t |= section_traits(s.palette[i]);
    return t;
}

// ChunkSkyLightSources.findLowestSourceY
i32 find_lowest_source(const Chunk& c, const Layout& l, i32 top_section, u32 lx, u32 lz) {
    i32 top_y = (l.min_section + top_section + 1) * 16;  // the block whose state is `top`
    BlockState top = kAir;
    for (i32 si = top_section; si >= 0; --si) {
        const Section& s = c.sections[si];
        if (s.only_air()) {
            top = kAir;
            top_y = (l.min_section + si) * 16;
            continue;
        }
        for (i32 ly = 15; ly >= 0; --ly) {
            const BlockState bottom = s.get(lx, static_cast<u32>(ly), lz);
            if (sky_edge_occluded(top, bottom)) return top_y;
            top = bottom;
            --top_y;
        }
    }
    return l.min_y - 1;
}

}  // namespace

u32 section_traits(BlockState s) {
    const u32 p = physics::kState[s];
    const physics::Material& m = physics::kMaterials[p >> physics::kMaterialShift];
    return ((p & physics::kLarge) ? 1u : 0u) | ((m.inside != physics::kInsideNone || (m.flags & physics::kStepSlime)) ? 2u : 0u) |
           ((m.flags & physics::kFallResetting) ? 4u : 0u);
}

u32 fill_section_masks(Chunk& c) {
    u32 any = 0;
    c.filled_sections = c.large_sections = c.entity_sections = c.fall_sections = 0;
    for (u32 si = 0; si < c.sections_count; ++si) {
        const u32 traits = traits_of_section(c.sections[si]), bit = 1u << si;
        if (!c.sections[si].only_air()) c.filled_sections |= bit;
        if (traits & 1) c.large_sections |= bit;
        if (traits & 2) c.entity_sections |= bit;
        if (traits & 4) c.fall_sections |= bit;
        any |= traits;
    }
    return any & 3;
}

// Highest block per column matching each map's predicate, stored as (y + 1 - min_y), 0 when the
// column has none.
void prime_heightmaps(Chunk& c) {
    c.world_surface.fill(0);
    c.motion_blocking.fill(0);
    c.motion_blocking_no_leaves.fill(0);
    u8 need[256];
    std::memset(need, 7, sizeof need);
    u32 remaining = 3 * 256;
    BlockState states[Section::kVolume];
    for (i32 si = static_cast<i32>(c.sections_count) - 1; si >= 0 && remaining; --si) {
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

void fill_sky_sources(Chunk& c, const Layout& l) {
    const i32 base = l.min_y - 1;  // ChunkSkyLightSources.minY
    i32 top = static_cast<i32>(l.sections) - 1;
    while (top >= 0 && c.sections[top].only_air()) --top;  // getHighestFilledSectionIndex
    if (top < 0) {
        c.sky_sources.fill(0);
        return;
    }
    for (u32 z = 0; z < 16; ++z)
        for (u32 x = 0; x < 16; ++x)
            c.sky_sources.set(x + z * 16, static_cast<u32>(std::max(find_lowest_source(c, l, top, x, z), base) - base));
}

u32 derive(Chunk& c, const Layout& l) {
    c.sections_count = static_cast<u8>(l.sections);
    c.light_count = static_cast<u8>(l.sections + 2);
    const u32 any = fill_section_masks(c);
    prime_heightmaps(c);
    if (l.has_skylight) fill_sky_sources(c, l);
    return any;
}

}  // namespace world
