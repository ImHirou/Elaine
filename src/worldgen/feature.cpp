// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/feature.hpp"

#include <cmath>

#include "phys/mth.hpp"
#include "worldgen/jmath.hpp"
#include "worldgen/material.hpp"
#include "worldgen/mth.hpp"

namespace worldgen {

namespace hm = world::heightmap;

namespace {

// An int sum as Java makes it: one that wraps.
i32 wrapping_add(i32 a, i32 b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }

// Mth.nextInt(random, min, max): nothing is drawn where there is nothing to choose.
i32 next_between(FeatureRandom& random, i32 min, i32 max) { return min >= max ? min : random.next_int(max - min + 1) + min; }

// WeightedList.getRandomOrThrow: the entry a draw below the sum of the weights falls into.
template <usize N>
u32 pick(const ft::Entry (&entries)[N], u32 first, u32 count, i32 total, FeatureRandom& random) {
    i32 selection = random.next_int(total);
    for (u32 i = first; i < first + count; ++i) {
        selection -= entries[i].weight;
        if (selection < 0) return entries[i].value;
    }
    DCHECK(false);
    return entries[first].value;
}

}  // namespace

i32 sample(const ft::Int& p, FeatureRandom& random) {
    switch (p.op) {
        case ft::IntOp::kConstant: return p.a;
        case ft::IntOp::kUniform:  // Mth.randomBetweenInclusive
            return random.next_int(p.b - p.a + 1) + p.a;
        case ft::IntOp::kTrapezoid: {
            if (p.c == 0 && p.b == -p.a) {
                const i32 first = random.next_int(p.b + 1);
                return first - random.next_int(p.b + 1);
            }
            const i32 range = p.b - p.a;
            if (p.c == range) return random.next_int(range + 1) + p.a;
            const i32 plateau_start = (range - p.c) / 2, plateau_end = range - plateau_start;
            const i32 first = random.next_int(plateau_end + 1);
            return p.a + first + random.next_int(plateau_start + 1);
        }
        case ft::IntOp::kWeightedList:
            return sample(ft::kInts[pick(ft::kIntEntries, static_cast<u32>(p.a), static_cast<u32>(p.b), p.c, random)], random);
        case ft::IntOp::kClamped:  // Mth.clamp of ints
            return std::min(std::max(sample(ft::kInts[p.c], random), p.a), p.b);
        case ft::IntOp::kClampedNormal: {
            // Mth.normal, then Mth.clamp of floats: all of it in float.
            const float value = p.fa + static_cast<float>(random.next_gaussian()) * p.fb;
            const float min = static_cast<float>(p.a), max = static_cast<float>(p.b);
            return static_cast<i32>(value < min ? min : std::min(value, max));
        }
        case ft::IntOp::kBiasedToBottom: {
            const i32 limit = random.next_int(p.b - p.a + 1);
            return p.a + random.next_int(limit + 1);
        }
        case ft::IntOp::kVeryBiasedToBottom: {
            const i32 outer = random.next_int(p.b - p.a + 1);
            const i32 limit = random.next_int(outer + 1);
            return p.a + random.next_int(limit + 1);
        }
    }
    return 0;
}

i32 sample(const ft::Height& p, FeatureRandom& random) {
    switch (p.op) {
        case ft::HeightOp::kConstant: return p.min;
        case ft::HeightOp::kUniform:
            if (p.min > p.max) return p.min;
            return random.next_int(p.max - p.min + 1) + p.min;
        case ft::HeightOp::kTrapezoid: {
            if (p.min > p.max) return p.min;
            const i32 range = p.max - p.min;
            if (p.inner >= range) return random.next_int(range + 1) + p.min;
            const i32 plateau_start = (range - p.inner) / 2, plateau_end = range - plateau_start;
            const i32 first = random.next_int(plateau_end + 1);
            return p.min + first + random.next_int(plateau_start + 1);
        }
        case ft::HeightOp::kBiasedToBottom: {
            if (p.max - p.min - p.inner + 1 <= 0) return p.min;
            const i32 limit = random.next_int(p.max - p.min - p.inner + 1);
            return random.next_int(limit + p.inner) + p.min;
        }
        case ft::HeightOp::kVeryBiasedToBottom: {
            if (p.max - p.min - p.inner + 1 <= 0) return p.min;
            const i32 upper = next_between(random, p.min + p.inner, p.max);
            const i32 biased = next_between(random, p.min, upper - 1);
            return next_between(random, p.min, biased - 1 + p.inner);
        }
        case ft::HeightOp::kWeightedList:
            return sample(
                ft::kHeights[pick(ft::kHeightEntries, static_cast<u32>(p.min), static_cast<u32>(p.max), p.inner, random)],
                random);
    }
    return 0;
}

bool test(Region& region, const ft::Predicate& p, Pos at) {
    switch (p.op) {
        case ft::PredicateOp::kStateIn: return in_state_set(p.a, region.block(at.x + p.dx, at.y + p.dy, at.z + p.dz));
        case ft::PredicateOp::kNot: return !test(region, ft::kPredicates[p.a], at);
        case ft::PredicateOp::kAllOf:
            for (u32 i = p.a; i < p.a + p.b; ++i)
                if (!test(region, ft::kPredicates[ft::kPredicateLists[i]], at)) return false;
            return true;
        case ft::PredicateOp::kAnyOf:
            for (u32 i = p.a; i < p.a + p.b; ++i)
                if (test(region, ft::kPredicates[ft::kPredicateLists[i]], at)) return true;
            return false;
        case ft::PredicateOp::kInsideWorld: return !Region::outside_height(at.y + p.dy);
        case ft::PredicateOp::kVolume: {
            const ft::Predicate& match = ft::kPredicates[p.a];
            for (i32 x = p.dx; x <= p.to[0]; ++x)
                for (i32 z = p.dz; z <= p.to[2]; ++z)
                    for (i32 y = p.dy; y <= p.to[1]; ++y)
                        if (!test(region, match, {at.x + x, at.y + y, at.z + z})) return false;
            return true;
        }
        case ft::PredicateOp::kWouldSurvive:
            // (The block's own canSurvive: world/block_rules is still to come, and a placed feature
            // that asks is not ported until then.)
            DCHECK(false);
            return false;
    }
    return false;
}

bool test(const ft::Rule& rule, u32 state, i32 y) {
    switch (rule.op) {
        case ft::RuleOp::kStateIn: return in_state_set(static_cast<u32>(rule.a), state);
        case ft::RuleOp::kHeight: return rule.a <= y && y <= rule.b;
        case ft::RuleOp::kNot: return !test(ft::kRules[rule.a], state, y);
        case ft::RuleOp::kAllOf:
            for (i32 i = rule.a; i < rule.a + rule.b; ++i)
                if (!test(ft::kRules[ft::kRuleLists[i]], state, y)) return false;
            return true;
        case ft::RuleOp::kAnyOf:
            for (i32 i = rule.a; i < rule.a + rule.b; ++i)
                if (test(ft::kRules[ft::kRuleLists[i]], state, y)) return true;
            return false;
    }
    return false;
}

void Decorator::modify(const ft::Modifier& m, FeatureRandom& random, Pos at, const Top* top, std::vector<Pos>& out) {
    Region& region = region_;
    switch (m.op) {
        case ft::ModifierOp::kCount: {
            const i32 count = sample(ft::kInts[m.a], random);
            for (i32 i = 0; i < count; ++i) out.push_back(at);
            return;
        }
        case ft::ModifierOp::kInSquare: {
            const i32 x = random.next_int(16) + at.x;
            const i32 z = random.next_int(16) + at.z;
            out.push_back({x, at.y, z});
            return;
        }
        case ft::ModifierOp::kHeightRange: out.push_back({at.x, sample(ft::kHeights[m.a], random), at.z}); return;
        case ft::ModifierOp::kHeightmap: {
            const i32 height = region.height(static_cast<u32>(m.a), at.x, at.z);
            if (height > Region::kMinY) out.push_back({at.x, height, at.z});
            return;
        }
        case ft::ModifierOp::kBiome: {
            DCHECK(top != nullptr);  // (vanilla throws: gen_features.py stops at a feature within a feature that asks)
            if (!top) return;
            const u32 biome = region.biome(at.x, at.y, at.z);
            if (ft::kBiomeFeatures[biome][top->step][top->index >> 6] >> (top->index & 63) & 1) out.push_back(at);
            return;
        }
        case ft::ModifierOp::kRarityFilter:
            if (random.next_float() < 1.0f / static_cast<float>(m.a)) out.push_back(at);
            return;
        case ft::ModifierOp::kRandomChance:
            if (random.next_float() < m.f) out.push_back(at);
            return;
        case ft::ModifierOp::kBlockPredicateFilter:
            if (test(region, ft::kPredicates[m.a], at)) out.push_back(at);
            return;
        case ft::ModifierOp::kOffset: {
            const i32 x = sample(ft::kInts[m.a], random);
            const i32 y = sample(ft::kInts[m.b], random);
            const i32 z = sample(ft::kInts[m.c], random);
            out.push_back({at.x + x, at.y + y, at.z + z});
            return;
        }
        case ft::ModifierOp::kEnvironmentScan: {
            const ft::Predicate& target = ft::kPredicates[m.a];
            const ft::Predicate& allowed = ft::kPredicates[m.b];
            Pos pos = at;
            if (!test(region, allowed, pos)) return;
            for (u32 i = 0; i < m.d; ++i) {
                if (test(region, target, pos)) {
                    out.push_back(pos);
                    return;
                }
                pos.y += m.flags & 1 ? 1 : -1;
                if (Region::outside_height(pos.y)) return;
                if (!test(region, allowed, pos)) break;
            }
            if (test(region, target, pos)) out.push_back(pos);
            return;
        }
        case ft::ModifierOp::kSurfaceWaterDepthFilter: {
            const i32 floor = region.height(hm::kOceanFloor, at.x, at.z);
            const i32 surface = region.height(hm::kWorldSurface, at.x, at.z);
            if (surface - floor <= m.a) out.push_back(at);
            return;
        }
        case ft::ModifierOp::kSurfaceRelativeThresholdFilter: {
            // (Where no bound is written it is an int's own, and the sum wraps as Java's does.)
            const i32 surface = region.height(static_cast<u32>(m.a), at.x, at.z);
            if (wrapping_add(surface, m.b) <= at.y && at.y <= wrapping_add(surface, m.c)) out.push_back(at);
            return;
        }
        case ft::ModifierOp::kNoiseThresholdCount: {
            const double noise =
                static_cast<double>(biome_info_noise(static_cast<double>(at.x) / 200.0, static_cast<double>(at.z) / 200.0));
            const i32 count = noise < ft::kDoubles[m.d] ? m.a : m.b;
            for (i32 i = 0; i < count; ++i) out.push_back(at);
            return;
        }
        case ft::ModifierOp::kNoiseBasedCount: {
            const double factor = ft::kDoubles[m.d], offset = ft::kDoubles[m.d + 1];
            const double noise =
                static_cast<double>(biome_info_noise(static_cast<double>(at.x) / factor, static_cast<double>(at.z) / factor));
            const i32 count = static_cast<i32>(std::ceil((noise + offset) * static_cast<double>(m.a)));
            for (i32 i = 0; i < count; ++i) out.push_back(at);
            return;
        }
        case ft::ModifierOp::kCuboid: {
            const i32 height = sample(ft::kInts[m.b], random);
            const i32 width = sample(ft::kInts[m.a], random);
            const i32 length = sample(ft::kInts[m.a], random);
            const bool edges = m.flags & 1, inside = m.flags & 2;
            for (i32 x = 0; x <= width; ++x)
                for (i32 y = 0; y <= height; ++y)
                    for (i32 z = 0; z <= length; ++z) {
                        const bool on_x = x == 0 || x == width, on_y = y == 0 || y == height, on_z = z == 0 || z == length;
                        if (!edges && ((on_x && on_y) || (on_z && on_y) || (on_x && on_z))) continue;
                        if (!inside && !(on_x || on_y || on_z)) continue;
                        out.push_back({at.x + x, at.y + y, at.z + z});
                    }
            return;
        }
        case ft::ModifierOp::kRandomlySelected:  // Util.getRandom
            modify(ft::kModifiers[static_cast<u32>(m.a) + static_cast<u32>(random.next_int(m.b))], random, at, top, out);
            return;
    }
}

bool Decorator::place(u32 placed_at, FeatureRandom& random, Pos origin, const Top* top) {
    const ft::Placed& placed = ft::kPlaced[placed_at];
    const bool record = record_ != nullptr && top != nullptr;
    const auto feature = [&](Pos at) {
        if (record) {
            record_->push_back(at);
            return true;
        }
        return place_feature(placed.feature, random, at);
    };
    if (placed.modifiers == 0) return feature(origin);
    FeatureWork& w = work_;
    // A feature placed here may place others the same way: each works above what its caller left.
    const usize waiting = w.waiting.size(), made = w.made.size();
    bool any = false;
    w.waiting.push_back(origin);
    w.waiting_at.push_back(0);
    while (w.waiting.size() > waiting) {
        const Pos at = w.waiting.back();
        const u32 index = w.waiting_at.back();
        w.waiting.pop_back();
        w.waiting_at.pop_back();
        modify(ft::kModifiers[placed.first + index], random, at, top, w.made);
        if (index + 1 < placed.modifiers) {
            for (usize i = w.made.size(); i-- > made;) {
                w.waiting.push_back(w.made[i]);
                w.waiting_at.push_back(static_cast<u16>(index + 1));
            }
        } else {
            const usize end = w.made.size();
            for (usize i = made; i < end; ++i) {
                const Pos next = w.made[i];
                any |= feature(next);
            }
        }
        w.made.resize(made);
    }
    return any;
}

bool Decorator::place_feature(u32 feature, FeatureRandom& random, Pos at) {
    const ft::Feature& f = ft::kFeatures[feature];
    switch (f.type) {
        case ft::FeatureType::kUnported: return false;
        case ft::FeatureType::kNoOp: return true;
        case ft::FeatureType::kRandomSelector:
            for (u32 i = f.a; i < f.a + f.b; ++i)
                if (random.next_float() < ft::kChances[i].chance) return place(ft::kChances[i].placed, random, at, nullptr);
            return place(f.c, random, at, nullptr);
        case ft::FeatureType::kSimpleRandomSelector:
            return place(ft::kPlacedLists[f.a + static_cast<u32>(random.next_int(static_cast<i32>(f.b)))], random, at, nullptr);
        case ft::FeatureType::kWeightedRandomSelector:
            if (f.b == 0) return false;
            return place(pick(ft::kWeighted, f.a, f.b, static_cast<i32>(f.c), random), random, at, nullptr);
        case ft::FeatureType::kRandomBooleanSelector: return place(random.next_boolean() ? f.a : f.b, random, at, nullptr);
        case ft::FeatureType::kSequence:
            for (u32 i = f.a; i < f.a + f.b; ++i)
                if (!place(ft::kPlacedLists[i], random, at, nullptr)) return false;
            return true;
        case ft::FeatureType::kOverlay: {
            bool any = false;
            for (u32 i = f.a; i < f.a + f.b; ++i) any |= place(ft::kPlacedLists[i], random, at, nullptr);
            return any;
        }
        case ft::FeatureType::kOre: return place_ore(ft::kOres[f.a], random, at);
    }
    return false;
}

namespace {

// Mth.ceil(float)
i32 ceil_int(float v) {
    const i32 truncated = static_cast<i32>(v);
    return v > static_cast<float>(truncated) ? truncated + 1 : truncated;
}

// java.util.BitSet as OreFeature.doPlace uses one: bits that are set and asked for by a number
// that may lie beyond what the set was made for - it grows. The words are the thread's, kept
// from one ore to the next.
struct Tested {
    std::vector<u64>& words;

    Tested(std::vector<u64>& kept, u32 bits) : words(kept) { words.assign((bits + 63) >> 6, 0); }
    bool get(u32 bit) const { return bit >> 6 < words.size() && (words[bit >> 6] >> (bit & 63) & 1); }
    void set(u32 bit) {
        if (UNLIKELY(bit >> 6 >= words.size())) words.resize((bit >> 6) + 1, 0);
        words[bit >> 6] |= u64{1} << (bit & 63);
    }
};

}  // namespace

// OreFeature.place and doPlace: a blob of ore along a line through the origin - a string of
// spheres, each a random size, the middle ones larger - laid into whatever its targets say it may
// replace. It reads and writes the sections themselves (BulkSectionAccess): no heightmap moves.
bool Decorator::place_ore(const ft::Ore& ore, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    const i32 size = ore.size;
    const float dir = random.next_float() * 3.1415927f;
    const float spread_xy = static_cast<float>(size) / 8.0f;
    const i32 max_radius = ceil_int((static_cast<float>(size) / 16.0f * 2.0f + 1.0f) / 2.0f);
    const double sine = jmath::sin(static_cast<double>(dir)), cosine = jmath::cos(static_cast<double>(dir));
    const double x0 = static_cast<double>(origin.x) + sine * static_cast<double>(spread_xy);
    const double x1 = static_cast<double>(origin.x) - sine * static_cast<double>(spread_xy);
    const double z0 = static_cast<double>(origin.z) + cosine * static_cast<double>(spread_xy);
    const double z1 = static_cast<double>(origin.z) - cosine * static_cast<double>(spread_xy);
    const i32 first_y = random.next_int(3);
    const double y0 = static_cast<double>(origin.y + first_y - 2);
    const double y1 = static_cast<double>(origin.y + random.next_int(3) - 2);
    const i32 x_start = origin.x - ceil_int(spread_xy) - max_radius;
    const i32 y_start = origin.y - 2 - max_radius;
    const i32 z_start = origin.z - ceil_int(spread_xy) - max_radius;
    const i32 size_xz = 2 * (ceil_int(spread_xy) + max_radius), size_y = 2 * (2 + max_radius);
    // Only where the box the blob lies in reaches down to the ground somewhere.
    bool buried = false;
    for (i32 x = x_start; x <= x_start + size_xz && !buried; ++x)
        for (i32 z = z_start; z <= z_start + size_xz && !buried; ++z)
            buried = y_start <= region.height(world::heightmap::kOceanFloorWg, x, z);
    if (!buried) return false;

    double data[64 * 4];
    for (i32 i = 0; i < size; ++i) {
        const float step = static_cast<float>(i) / static_cast<float>(size);
        const double xx = lerp(static_cast<double>(step), x0, x1);
        const double yy = lerp(static_cast<double>(step), y0, y1);
        const double zz = lerp(static_cast<double>(step), z0, z1);
        const double ss = random.next_double() * static_cast<double>(size) / 16.0;
        const double r = (static_cast<double>(phys::mth_sin(static_cast<double>(3.1415927f * step)) + 1.0f) * ss + 1.0) / 2.0;
        data[i * 4 + 0] = xx;
        data[i * 4 + 1] = yy;
        data[i * 4 + 2] = zz;
        data[i * 4 + 3] = r;
    }
    // A sphere that lies wholly within another is not gone through.
    for (i32 i1 = 0; i1 < size - 1; ++i1) {
        if (data[i1 * 4 + 3] <= 0.0) continue;
        for (i32 i2 = i1 + 1; i2 < size; ++i2) {
            if (data[i2 * 4 + 3] <= 0.0) continue;
            const double dx = data[i1 * 4 + 0] - data[i2 * 4 + 0];
            const double dy = data[i1 * 4 + 1] - data[i2 * 4 + 1];
            const double dz = data[i1 * 4 + 2] - data[i2 * 4 + 2];
            const double dr = data[i1 * 4 + 3] - data[i2 * 4 + 3];
            if (dr * dr <= dx * dx + dy * dy + dz * dz) continue;
            if (dr > 0.0)
                data[i2 * 4 + 3] = -1.0;
            else
                data[i1 * 4 + 3] = -1.0;
        }
    }

    i32 placed = 0;
    Tested tested(work_.tested, static_cast<u32>(size_xz * size_y * size_xz));
    const float discard = ore.discard_chance_on_air_exposure;
    for (i32 i = 0; i < size; ++i) {
        const double r = data[i * 4 + 3];
        if (r < 0.0) continue;
        const double xx = data[i * 4 + 0], yy = data[i * 4 + 1], zz = data[i * 4 + 2];
        const i32 x_min = std::max(floor_int(xx - r), x_start), y_min = std::max(floor_int(yy - r), y_start),
                  z_min = std::max(floor_int(zz - r), z_start);
        const i32 x_max = std::max(floor_int(xx + r), x_min), y_max = std::max(floor_int(yy + r), y_min),
                  z_max = std::max(floor_int(zz + r), z_min);
        for (i32 x = x_min; x <= x_max; ++x) {
            const double xd = (static_cast<double>(x) + 0.5 - xx) / r;
            if (xd * xd >= 1.0) continue;
            for (i32 y = y_min; y <= y_max; ++y) {
                const double yd = (static_cast<double>(y) + 0.5 - yy) / r;
                if (xd * xd + yd * yd >= 1.0) continue;
                for (i32 z = z_min; z <= z_max; ++z) {
                    const double zd = (static_cast<double>(z) + 0.5 - zz) / r;
                    if (xd * xd + yd * yd + zd * zd >= 1.0) continue;
                    if (Region::outside_height(y)) continue;
                    const u32 bit = static_cast<u32>((x - x_start) + (y - y_start) * size_xz + (z - z_start) * size_xz * size_y);
                    if (tested.get(bit)) continue;
                    tested.set(bit);
                    if (!region.can_write(x, z)) continue;
                    const u16 state = region.raw(x, y, z);
                    for (u32 t = ore.first; t < static_cast<u32>(ore.first) + ore.targets; ++t) {
                        // AbstractOreFeature.canPlaceOre: the target's test; then, for an ore that shuns the
                        // open, a draw - and failing that, no air on any of its six sides.
                        if (!test(ft::kRules[ft::kOreTargets[t].rule], state, y)) continue;
                        bool can = true;
                        if (discard > 0.0f && (discard >= 1.0f || random.next_float() < discard)) {
                            can = !(world::is_air(region.raw(x, y - 1, z)) || world::is_air(region.raw(x, y + 1, z)) ||
                                    world::is_air(region.raw(x, y, z - 1)) || world::is_air(region.raw(x, y, z + 1)) ||
                                    world::is_air(region.raw(x - 1, y, z)) || world::is_air(region.raw(x + 1, y, z)));
                        }
                        if (!can) continue;
                        region.put_raw(x, y, z, ft::kOreTargets[t].state);
                        ++placed;
                        break;
                    }
                }
            }
        }
    }
    return placed > 0;
}

void Decorator::decorate() {
    Region& region = region_;
    // SectionPos.origin of the chunk at the world's lowest section: its corner, at the bottom.
    const Pos origin{region.center_x() * 16, Region::kMinY, region.center_z() * 16};
    // (One source for the whole of it, as vanilla has: a second number of a gaussian that a
    // feature left undrawn is the next feature's first.)
    FeatureRandom random{XoroshiroRandom{u64{0}, u64{0}}};
    const i64 decoration_seed = random.set_decoration_seed(region.world().seed(), origin.x, origin.z);
    u64 biomes[2];
    region.possible_biomes(biomes);
    biomes[0] &= ft::kPossibleBiomes[0];
    biomes[1] &= ft::kPossibleBiomes[1];
    for (u32 step = 0; step < ft::kSteps; ++step) {
        // The features of every biome that is somewhere in the nine chunks, each once, by index.
        u64 has[ft::kStepWords] = {};
        for (u32 half = 0; half < 2; ++half)
            for (u64 bits = biomes[half]; bits; bits &= bits - 1) {
                const u32 biome = half * 64 + static_cast<u32>(std::countr_zero(bits));
                for (u32 word = 0; word < ft::kStepWords; ++word) has[word] |= ft::kBiomeFeatures[biome][step][word];
            }
        for (u32 word = 0; word < ft::kStepWords; ++word)
            for (u64 bits = has[word]; bits; bits &= bits - 1) {
                const u32 index = word * 64 + static_cast<u32>(std::countr_zero(bits));
                const u32 slot = ft::kStepFirst[step] + index;
                const u32 placed = ft::kStepPlaced[slot];
                if (!only_.empty() && !only_[slot]) continue;
                if (record_ ? !ft::kPlaced[placed].where : !ft::kPlaced[placed].ported) continue;
                random.set_feature_seed(decoration_seed, static_cast<i32>(index), static_cast<i32>(step));
                const Top top{step, index};
                if (log_) log_->push_back(top);
                u64 writes = 0xcbf29ce484222325ull;
                if (trace_) region.set_log(&writes);
                place(placed, random, origin, &top);
                if (trace_) {
                    region.set_log(nullptr);
                    // (The source is the feature's own until the next is seeded: a long more of it moves nothing.)
                    trace_->push_back({step, index, writes, random.next_long()});
                }
            }
    }
    if (!record_) region.center().decorated = true;  // (ChunkStatus.FEATURES)
}

}  // namespace worldgen
