// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/feature.hpp"

#include <algorithm>
#include <cmath>

#include "phys/mth.hpp"
#include "worldgen/jmath.hpp"
#include "worldgen/material.hpp"
#include "worldgen/mth.hpp"
#include "worldgen/survive.hpp"

namespace worldgen {

namespace hm = world::heightmap;
namespace blk = proto::gen::block;

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
        case ft::PredicateOp::kWouldSurvive: return survives(region, ft::kSurvive[p.a], at.x + p.dx, at.y + p.dy, at.z + p.dz);
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
        case ft::FeatureType::kDisk: return place_disk(ft::kDisks[f.a], random, at);
        case ft::FeatureType::kUnderwaterMagma: return place_underwater_magma(ft::kMagmas[f.a], random, at);
        case ft::FeatureType::kBlockBlob: return place_block_blob(static_cast<u16>(f.a), ft::kPredicates[f.b], random, at);
        case ft::FeatureType::kSpring: return place_spring(ft::kSprings[f.a], at);
        case ft::FeatureType::kLake: return place_lake(ft::kLakes[f.a], random, at);
        case ft::FeatureType::kBlueIce: return place_blue_ice(random, at);
        case ft::FeatureType::kSpike:
            return place_spike(static_cast<u16>(f.a), ft::kPredicates[f.b], ft::kPredicates[f.c], random, at);
        case ft::FeatureType::kIceberg: return place_iceberg(static_cast<u16>(f.a), random, at);
        case ft::FeatureType::kFreezeTopLayer: return place_freeze_top_layer(at);
        case ft::FeatureType::kSimpleBlock: return place_simple_block(f, random, at);
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

// ---------------------------------------------------------------------------- what the features share

namespace {

// state.is(Blocks.WATER), state.is(Blocks.SNOW): any state of the block.
bool is_water(u16 state) { return world::block_of(state) == world::block_of(blk::kWater); }
bool is_snow_layer(u16 state) { return world::block_of(state) == world::block_of(blk::kSnow); }

// FluidTags.WATER: water, running or not.
constexpr u32 kWaterFamily = proto::gen::fluid::kFamily[proto::gen::fluid::kWater];

// LevelAccessor.getBrightness(LightLayer.BLOCK, pos) while a chunk is decorated: no light has been
// let into it yet (docs/architecture.md, "Features").
constexpr i32 kBlockLight = 0;

// Biome.shouldFreeze(level, pos, false): cold, and a source of water in a block of water.
bool should_freeze(Region& region, u16 biome, i32 x, i32 y, i32 z) {
    if (warm_enough_to_rain(biome, x, y, z)) return false;
    if (Region::outside_height(y) || kBlockLight >= 10) return false;
    const u16 state = region.block(x, y, z);
    return world::holds_water_source(state) && is_water(state);
}

// Biome.shouldSnow: snow falls there, and a layer of it can lie on what is below.
bool should_snow(Region& region, u16 biome, i32 x, i32 y, i32 z) {
    if (!has_precipitation(biome) || warm_enough_to_rain(biome, x, y, z)) return false;
    if (Region::outside_height(y) || kBlockLight >= 10) return false;
    const u16 state = region.block(x, y, z);
    if (!world::is_air(state) && !is_snow_layer(state)) return false;
    return in_state_set(ft::kSetSnowSurvivesOn, region.block(x, y - 1, z));
}

}  // namespace

namespace {

// The noises of the state providers that go by one (NoiseBasedStateProvider): made of a fixed seed, so
// the same in every world - laid out once as the layers they are.
const Noise& provider_noise(u32 first, u32 count) {
    static const std::vector<Layer> layers = [] {
        std::vector<Layer> made(std::size(ft::kProviderLayers));
        for (usize i = 0; i < made.size(); ++i) {
            const ft::ProviderLayer& from = ft::kProviderLayers[i];
            made[i].noise.offset_x = from.offsets[0];
            made[i].noise.offset_y = from.offsets[1];
            made[i].noise.offset_z = from.offsets[2];
            made[i].noise.fudge_y_scale = 0.0;
            std::copy(std::begin(from.perms), std::end(from.perms), made[i].noise.perms);
            made[i].frequency = from.frequency;
            made[i].amplitude = from.amplitude;
        }
        return made;
    }();
    static const std::vector<Noise> noises = [] {
        // (One for every run of layers that a provider names.)
        std::vector<Noise> made;
        for (const ft::NoiseProvider& p : ft::kNoiseProviders) {
            made.push_back({layers.data() + p.noise, p.layers, false});
            made.push_back({layers.data() + p.slow, p.slow_layers, false});
        }
        return made;
    }();
    for (const Noise& n : noises)
        if (n.layers == layers.data() + first && n.count == count) return n;
    return noises[0];
}

// NoiseProvider.getRandomState(states, noiseValue): which of so many states a noise value picks.
u32 by_noise(float noise_value, u32 count) {
    const float v = (1.0f + noise_value) / 2.0f;
    const float placement = v < 0.0f ? 0.0f : std::min(v, 0.9999f);
    return static_cast<u32>(static_cast<i32>(placement * static_cast<float>(count)));
}

// Mth.clampedMap
double clamped_map(double value, double from_min, double from_max, double to_min, double to_max) {
    const double factor = (value - from_min) / (from_max - from_min);
    if (factor < 0.0) return to_min;
    if (factor > 1.0) return to_max;
    return lerp(factor, to_min, to_max);
}

}  // namespace

// NoiseProvider, NoiseThresholdProvider, DualNoiseProvider: the flowers of a field go by where they are.
u16 Decorator::noise_state(const ft::NoiseProvider& p, FeatureRandom& random, Pos at) {
    const Noise& noise = provider_noise(p.noise, p.layers);
    const double scale = static_cast<double>(p.scale);
    // NoiseBasedStateProvider.getNoiseValue
    const float value =
        noise.get(static_cast<double>(at.x) * scale, static_cast<double>(at.y) * scale, static_cast<double>(at.z) * scale);
    switch (p.kind) {
        case ft::NoiseKind::kNoise: return ft::kProviderStates[p.states + by_noise(value, p.state_count)];
        case ft::NoiseKind::kNoiseThreshold: {
            if (static_cast<double>(value) < static_cast<double>(p.threshold))
                return ft::kProviderStates[p.low + static_cast<u32>(random.next_int(static_cast<i32>(p.low_count)))];
            if (random.next_float() < p.high_chance)
                return ft::kProviderStates[p.high + static_cast<u32>(random.next_int(static_cast<i32>(p.high_count)))];
            return p.default_state;
        }
        case ft::NoiseKind::kDualNoise: {
            // A slower noise says how many kinds grow here, and which: each from a place of its own on it.
            const Noise& slow = provider_noise(p.slow, p.slow_layers);
            const auto slow_value = [&](i32 x, i32 y, i32 z) {
                return slow.get(static_cast<double>(static_cast<float>(x) * p.slow_scale),
                                static_cast<double>(static_cast<float>(y) * p.slow_scale),
                                static_cast<double>(static_cast<float>(z) * p.slow_scale));
            };
            const double variety_noise = static_cast<double>(slow_value(at.x, at.y, at.z));
            const i32 variety = static_cast<i32>(clamped_map(variety_noise, -1.0, 1.0, static_cast<double>(p.variety_min),
                                                             static_cast<double>(p.variety_max + 1)));
            u16 possible[64];
            DCHECK(variety > 0 && variety <= 64);
            for (i32 i = 0; i < variety; ++i)
                possible[i] =
                    ft::kProviderStates[p.states + by_noise(slow_value(at.x + i * 54545, at.y, at.z + i * 34234), p.state_count)];
            return possible[by_noise(value, static_cast<u32>(variety))];
        }
    }
    return 0;
}

// BlockStateProvider.getOptionalState: a state, or none (-1).
i32 Decorator::optional_state(const ft::Provider& p, FeatureRandom& random, Pos at) {
    switch (p.op) {
        case ft::ProviderOp::kSimple: return static_cast<i32>(p.a);
        case ft::ProviderOp::kRuleBased:
            for (u32 i = p.a; i < p.a + p.b; ++i) {
                const ft::ProviderRule& rule = ft::kProviderRules[i];
                if (!test(region_, ft::kPredicates[rule.predicate], at)) continue;
                const i32 state = optional_state(ft::kProviders[rule.provider], random, at);
                if (state >= 0) return state;
            }
            return p.c ? optional_state(ft::kProviders[p.c - 1], random, at) : -1;
        case ft::ProviderOp::kWeighted:
            return static_cast<i32>(pick(ft::kProviderEntries, p.a, p.b, static_cast<i32>(p.c), random));
        case ft::ProviderOp::kRotated: {
            // RotatedBlockProvider: the direction first (Direction.getRandom), then the state it turns.
            const u32 direction = p.d ? p.d - 1 : static_cast<u32>(random.next_int(6));
            const u16 inner = state_of(ft::kProviders[p.a], random, at);
            for (u32 i = p.b; i < p.b + p.c; ++i)
                if (ft::kRotations[i].state == inner) return ft::kRotations[i].to[direction];
            return inner;
        }
        case ft::ProviderOp::kRandomizedInt: {
            // RandomizedIntStateProvider: a number is drawn only for a state that has the property.
            const u16 inner = state_of(ft::kProviders[p.a], random, at);
            for (u32 i = p.b; i < p.b + p.c; ++i) {
                const ft::IntStates& row = ft::kIntStates[i];
                if (row.state != inner) continue;
                const i32 value = sample(ft::kInts[p.d], random);
                DCHECK(value >= row.min && value < row.min + row.count);
                return ft::kProviderStates[row.first + static_cast<u32>(value - row.min)];
            }
            return inner;
        }
        case ft::ProviderOp::kRandomBlock:  // HolderSet.getRandomElement
            return p.b ? ft::kProviderStates[p.a + static_cast<u32>(random.next_int(static_cast<i32>(p.b)))] : -1;
        case ft::ProviderOp::kNoise: return noise_state(ft::kNoiseProviders[p.a], random, at);
        case ft::ProviderOp::kUnported: break;
    }
    DCHECK(false);  // (a feature with a provider that is not ported is not placed)
    return -1;
}

// BlockStateProvider.getState: where a provider of rules has no state, what is there stays.
u16 Decorator::state_of(const ft::Provider& p, FeatureRandom& random, Pos at) {
    const i32 state = optional_state(p, random, at);
    return state >= 0 ? static_cast<u16>(state) : region_.block(at.x, at.y, at.z);
}

// Feature.markAboveForPostProcessing: the two blocks above, as far as there is no air.
void Decorator::mark_above(i32 x, i32 y, i32 z) {
    for (i32 i = 0; i < 2; ++i) {
        ++y;
        if (world::is_air(region_.block(x, y, z))) return;
        region_.mark_post_process(x, y, z);
    }
}

// SimpleBlockFeature: one block of what a provider says, if that could stay there - a plant of two
// blocks with both its halves.
bool Decorator::place_simple_block(const ft::Feature& f, FeatureRandom& random, Pos o) {
    Region& region = region_;
    const i32 made = optional_state(ft::kProviders[f.a], random, o);
    if (made < 0) return false;
    const ft::SimpleBlock* row = nullptr;
    for (u32 i = f.b & 0xFFFFF; i < (f.b & 0xFFFFF) + (f.b >> 20) && !row; ++i)
        if (ft::kSimpleBlocks[i].state == made) row = &ft::kSimpleBlocks[i];
    DCHECK(row);  // (the table has every state the provider can give)
    if (!row || !survives(region, ft::kSurvive[row->survive], o.x, o.y, o.z)) return false;
    if (row->halves[0] | row->halves[1]) {
        const u16 above = region.block(o.x, o.y + 1, o.z);
        if (!world::is_air(above) && !world::is_replaceable(above)) return false;
        // DoublePlantBlock.placeAt: each half as it is where it stands - in water, or not (copyWaterloggedFrom).
        const auto water_at = [&](i32 y) {
            return world::fluid_family(world::fluid_of(region.block(o.x, y, o.z))) == kWaterFamily;
        };
        region.set_block(o.x, o.y, o.z, row->halves[water_at(o.y) ? 2 : 0], 2);
        region.set_block(o.x, o.y + 1, o.z, row->halves[water_at(o.y + 1) ? 3 : 1], 2);
    } else {
        region.set_block(o.x, o.y, o.z, static_cast<u16>(made), 2);
    }
    if (f.c) region.schedule_tick(o.x, o.y, o.z, world::block_of(region.block(o.x, o.y, o.z)), false);
    return true;
}

// DiskFeature: a round patch, a few blocks thick, of what its provider says wherever its target is.
bool Decorator::place_disk(const ft::Disk& disk, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    bool placed_any = false;
    const i32 top = origin.y + disk.half_height, bottom = origin.y - disk.half_height - 1;
    const i32 r = sample(ft::kInts[disk.radius], random);
    const ft::Predicate& target = ft::kPredicates[disk.target];
    const ft::Provider& provider = ft::kProviders[disk.provider];
    for (i32 z = origin.z - r; z <= origin.z + r; ++z) {  // (BlockPos.betweenClosed: x first)
        for (i32 x = origin.x - r; x <= origin.x + r; ++x) {
            const i32 xd = x - origin.x, zd = z - origin.z;
            if (xd * xd + zd * zd > r * r) continue;
            // placeColumn, from the top down
            bool placed_above = false;
            for (i32 y = top; y > bottom; --y) {
                if (!test(region, target, {x, y, z})) {
                    placed_above = false;
                    continue;
                }
                const i32 state = optional_state(provider, random, {x, y, z});
                if (state < 0) continue;
                region.set_block(x, y, z, static_cast<u16>(state), 2);
                if (!placed_above) mark_above(x, y, z);
                placed_any = true;
                placed_above = true;
            }
        }
    }
    return placed_any;
}

// UnderwaterMagmaFeature: magma blocks in the floor under a column of water, where they are
// walled in on every side but the top.
bool Decorator::place_underwater_magma(const ft::Magma& magma, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    // Column.scan, downwards: the floor is where the water ends, if that is within reach.
    if (!is_water(region.block(origin.x, origin.y, origin.z))) return false;
    i32 floor = origin.y;
    for (i32 i = 1; i < magma.floor_search_range && is_water(region.block(origin.x, floor, origin.z)); ++i) --floor;
    if (is_water(region.block(origin.x, floor, origin.z))) return false;
    // BlockState.getFaceOcclusionShape(direction) is not a whole face: what is behind it shows.
    const auto visible = [&](i32 x, i32 y, i32 z, world::Direction covered) {
        return !in_state_set(ft::kSetFaceOccludes[static_cast<u32>(covered)], region.block(x, y, z));
    };
    const i32 r = magma.placement_radius_around_floor;
    i32 placed = 0;
    for (i32 z = origin.z - r; z <= origin.z + r; ++z) {  // (BlockPos.betweenClosedStream: x first, then y)
        for (i32 y = floor - r; y <= floor + r; ++y) {
            for (i32 x = origin.x - r; x <= origin.x + r; ++x) {
                if (!(random.next_float() < magma.placement_probability_per_valid_position)) continue;
                // isValidPlacement
                const u16 state = region.block(x, y, z);
                if (is_water(state) || world::is_air(state) || visible(x, y - 1, z, world::kUp)) continue;
                if (visible(x, y, z - 1, world::kSouth) || visible(x + 1, y, z, world::kWest) ||
                    visible(x, y, z + 1, world::kNorth) || visible(x - 1, y, z, world::kEast))
                    continue;
                region.set_block(x, y, z, blk::kMagmaBlock, 2);
                ++placed;
            }
        }
    }
    return placed > 0;
}

// BlockBlobFeature: three lumps of a block, each beside the last, on the first ground below
// that it may lie on (the mossy rocks of an old forest).
bool Decorator::place_block_blob(u16 state, const ft::Predicate& can_place_on, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    while (origin.y > Region::kMinY + 3 && !test(region, can_place_on, {origin.x, origin.y - 1, origin.z})) --origin.y;
    if (origin.y <= Region::kMinY + 3) return false;
    for (i32 c = 0; c < 3; ++c) {
        const i32 xr = random.next_int(2);
        const i32 yr = random.next_int(2);
        const i32 zr = random.next_int(2);
        const float tr = static_cast<float>(xr + yr + zr) * 0.333f + 0.5f;
        for (i32 dz = -zr; dz <= zr; ++dz)
            for (i32 dy = -yr; dy <= yr; ++dy)
                for (i32 dx = -xr; dx <= xr; ++dx)
                    if (!(static_cast<double>(dx * dx + dy * dy + dz * dz) > static_cast<double>(tr * tr)))
                        region.set_block(origin.x + dx, origin.y + dy, origin.z + dz, state, 3);
        const i32 mx = -1 + random.next_int(2);
        const i32 my = -random.next_int(2);
        const i32 mz = -1 + random.next_int(2);
        origin = {origin.x + mx, origin.y + my, origin.z + mz};
    }
    return true;
}

// SpringFeature: a block of a fluid in a wall - rock on so many sides, air on so many - that
// begins to run when the chunk first ticks.
bool Decorator::place_spring(const ft::Spring& spring, Pos o) {
    Region& region = region_;
    const auto rock = [&](i32 x, i32 y, i32 z) { return in_state_set(spring.valid_blocks, region.block(x, y, z)); };
    const auto hole = [&](i32 x, i32 y, i32 z) { return world::is_air(region.block(x, y, z)); };
    if (!rock(o.x, o.y + 1, o.z)) return false;
    if (spring.requires_block_below && !rock(o.x, o.y - 1, o.z)) return false;
    const u16 current = region.block(o.x, o.y, o.z);
    if (!world::is_air(current) && !in_state_set(spring.valid_blocks, current)) return false;
    const i32 rocks = rock(o.x - 1, o.y, o.z) + rock(o.x + 1, o.y, o.z) + rock(o.x, o.y, o.z - 1) + rock(o.x, o.y, o.z + 1) +
                      rock(o.x, o.y - 1, o.z);
    const i32 holes = hole(o.x - 1, o.y, o.z) + hole(o.x + 1, o.y, o.z) + hole(o.x, o.y, o.z - 1) + hole(o.x, o.y, o.z + 1) +
                      hole(o.x, o.y - 1, o.z);
    if (rocks != spring.rock_count || holes != spring.hole_count) return false;
    region.set_block(o.x, o.y, o.z, spring.state, 2);
    region.schedule_tick(o.x, o.y, o.z, spring.fluid, true);
    return true;
}

// LakeFeature: a hollow of a few blobs in a box of 16 x 8 x 16, its lower half filled with a
// fluid, its upper half with air, and walled in with a barrier where the ground was solid.
bool Decorator::place_lake(const ft::Lake& lake, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    if (origin.y <= Region::kMinY + 4) return false;
    origin = {origin.x - 8, origin.y - 4, origin.z - 8};
    bool grid[2048] = {};
    const auto in = [&](i32 xx, i32 zz, i32 yy) -> bool& { return grid[(xx * 16 + zz) * 8 + yy]; };
    const i32 spots = random.next_int(4) + 4;
    for (i32 i = 0; i < spots; ++i) {
        const double xr = random.next_double() * 6.0 + 3.0;
        const double yr = random.next_double() * 4.0 + 2.0;
        const double zr = random.next_double() * 6.0 + 3.0;
        const double xp = random.next_double() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
        const double yp = random.next_double() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
        const double zp = random.next_double() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
        for (i32 xx = 1; xx < 15; ++xx)
            for (i32 zz = 1; zz < 15; ++zz)
                for (i32 yy = 1; yy < 7; ++yy) {
                    const double xd = (static_cast<double>(xx) - xp) / (xr / 2.0);
                    const double yd = (static_cast<double>(yy) - yp) / (yr / 2.0);
                    const double zd = (static_cast<double>(zz) - zp) / (zr / 2.0);
                    const double d = xd * xd + yd * yd + zd * zd;
                    if (d < 1.0) in(xx, zz, yy) = true;
                }
    }
    const u16 fluid = state_of(ft::kProviders[lake.fluid], random, origin);
    // A cell of the box that is not hollow and has a hollow one beside it, above or below it.
    const auto edge = [&](i32 xx, i32 zz, i32 yy) {
        return !in(xx, zz, yy) &&
               ((xx < 15 && in(xx + 1, zz, yy)) || (xx > 0 && in(xx - 1, zz, yy)) || (zz < 15 && in(xx, zz + 1, yy)) ||
                (zz > 0 && in(xx, zz - 1, yy)) || (yy < 7 && in(xx, zz, yy + 1)) || (yy > 0 && in(xx, zz, yy - 1)));
    };
    const ft::Predicate& can_place = ft::kPredicates[lake.can_place_feature];
    for (i32 xx = 0; xx < 16; ++xx)
        for (i32 zz = 0; zz < 16; ++zz)
            for (i32 yy = 0; yy < 8; ++yy) {
                if (!edge(xx, zz, yy)) continue;
                const Pos at{origin.x + xx, origin.y + yy, origin.z + zz};
                const u16 state = region.block(at.x, at.y, at.z);
                if (yy >= 4 && in_state_set(ft::kSetLiquid, state)) return false;
                if (yy < 4 && !world::is_solid(state) && state != fluid) return false;
                if (!test(region, can_place, at)) return false;
            }
    const ft::Predicate& can_replace = ft::kPredicates[lake.can_replace_with_air_or_fluid];
    for (i32 xx = 0; xx < 16; ++xx)
        for (i32 zz = 0; zz < 16; ++zz)
            for (i32 yy = 0; yy < 8; ++yy) {
                if (!in(xx, zz, yy)) continue;
                const Pos at{origin.x + xx, origin.y + yy, origin.z + zz};
                if (!test(region, can_replace, at)) continue;
                const bool place_air = yy >= 4;
                region.set_block(at.x, at.y, at.z, place_air ? static_cast<u16>(blk::kCaveAir) : fluid, 2);
                if (place_air) {
                    region.schedule_tick(at.x, at.y, at.z, world::block_of(blk::kCaveAir), false);
                    mark_above(at.x, at.y, at.z);
                }
            }
    const u16 barrier = state_of(ft::kProviders[lake.barrier], random, origin);
    if (!world::is_air(barrier)) {
        const ft::Predicate& can_wall = ft::kPredicates[lake.can_replace_with_barrier];
        for (i32 xx = 0; xx < 16; ++xx)
            for (i32 zz = 0; zz < 16; ++zz)
                for (i32 yy = 0; yy < 8; ++yy) {
                    if (!edge(xx, zz, yy)) continue;
                    if (yy >= 4 && random.next_int(2) == 0) continue;
                    const Pos at{origin.x + xx, origin.y + yy, origin.z + zz};
                    if (!world::is_solid(region.block(at.x, at.y, at.z)) || !test(region, can_wall, at)) continue;
                    region.set_block(at.x, at.y, at.z, barrier, 2);
                    mark_above(at.x, at.y, at.z);
                }
    }
    // A lake of water has ice on it where it is cold.
    if (world::fluid_family(world::fluid_of(fluid)) == kWaterFamily) {
        for (i32 xx = 0; xx < 16; ++xx)
            for (i32 zz = 0; zz < 16; ++zz) {
                const Pos at{origin.x + xx, origin.y + 4, origin.z + zz};
                if (should_freeze(region, region.biome(at.x, at.y, at.z), at.x, at.y, at.z) && test(region, can_replace, at))
                    region.set_block(at.x, at.y, at.z, blk::kIce, 2);
            }
    }
    return true;
}

// BlueIceFeature: blue ice under the sea, grown from a block beside packed ice.
bool Decorator::place_blue_ice(FeatureRandom& random, Pos o) {
    Region& region = region_;
    static constexpr i32 kStep[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                        {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};  // Direction.values()
    if (o.y > settings::kSeaLevel - 1) return false;
    if (!is_water(region.block(o.x, o.y, o.z)) && !is_water(region.block(o.x, o.y - 1, o.z))) return false;
    bool packed = false;
    for (u32 d = 1; d < 6 && !packed; ++d)
        packed = region.block(o.x + kStep[d][0], o.y + kStep[d][1], o.z + kStep[d][2]) == blk::kPackedIce;
    if (!packed) return false;
    region.set_block(o.x, o.y, o.z, blk::kBlueIce, 2);
    for (i32 i = 0; i < 200; ++i) {
        const i32 up = random.next_int(5);
        const i32 y_off = up - random.next_int(6);
        i32 xz_diff = 3;
        if (y_off < 2) xz_diff += y_off / 2;
        if (xz_diff < 1) continue;
        const i32 x1 = random.next_int(xz_diff);
        const i32 x2 = random.next_int(xz_diff);
        const i32 z1 = random.next_int(xz_diff);
        const i32 z2 = random.next_int(xz_diff);
        const Pos at{o.x + x1 - x2, o.y + y_off, o.z + z1 - z2};
        const u16 state = region.block(at.x, at.y, at.z);
        if (!world::is_air(state) && !is_water(state) && state != blk::kPackedIce && state != blk::kIce) continue;
        for (const auto& step : kStep) {
            if (region.block(at.x + step[0], at.y + step[1], at.z + step[2]) != blk::kBlueIce) continue;
            region.set_block(at.x, at.y, at.z, blk::kBlueIce, 2);
            break;
        }
    }
    return true;
}

// SpikeFeature: a spike of ice on the snow - now and then a tall one - with a root below it.
bool Decorator::place_spike(u16 state, const ft::Predicate& can_place_on, const ft::Predicate& can_replace, FeatureRandom& random,
                            Pos origin) {
    Region& region = region_;
    while (world::is_air(region.block(origin.x, origin.y, origin.z)) && origin.y > Region::kMinY + 2) --origin.y;
    if (!test(region, can_place_on, origin)) return false;
    origin.y += random.next_int(4);
    const i32 height = random.next_int(4) + 7;
    const i32 width = height / 4 + random.next_int(2);
    if (width > 1 && random.next_int(60) == 0) origin.y += 10 + random.next_int(30);
    const auto put = [&](i32 x, i32 y, i32 z) {
        if (world::is_air(region.block(x, y, z)) || test(region, can_replace, {x, y, z})) region.set_block(x, y, z, state, 3);
    };
    for (i32 y_off = 0; y_off < height; ++y_off) {
        const float scale = (1.0f - static_cast<float>(y_off) / static_cast<float>(height)) * static_cast<float>(width);
        const i32 new_width = ceil_int(scale);
        for (i32 xo = -new_width; xo <= new_width; ++xo) {
            const float dx = static_cast<float>(std::abs(xo)) - 0.25f;
            for (i32 zo = -new_width; zo <= new_width; ++zo) {
                const float dz = static_cast<float>(std::abs(zo)) - 0.25f;
                if ((xo != 0 || zo != 0) && dx * dx + dz * dz > scale * scale) continue;
                if ((xo == -new_width || xo == new_width || zo == -new_width || zo == new_width) && random.next_float() > 0.75f)
                    continue;
                put(origin.x + xo, origin.y + y_off, origin.z + zo);
                if (y_off != 0 && new_width > 1) put(origin.x + xo, origin.y - y_off, origin.z + zo);
            }
        }
    }
    const i32 pillar = std::clamp(width - 1, 0, 1);
    for (i32 xo = -pillar; xo <= pillar; ++xo) {
        for (i32 zo = -pillar; zo <= pillar; ++zo) {
            i32 y = origin.y - 1;
            i32 run = 50;
            if (std::abs(xo) == 1 && std::abs(zo) == 1) run = random.next_int(5);
            while (y > 50) {
                const u16 there = region.block(origin.x + xo, y, origin.z + zo);
                if (!world::is_air(there) && !test(region, can_replace, {origin.x + xo, y, origin.z + zo}) && there != state)
                    break;
                region.set_block(origin.x + xo, y, origin.z + zo, state, 3);
                --y;
                if (--run <= 0) {
                    y -= random.next_int(5) + 1;
                    run = random.next_int(5);
                }
            }
        }
    }
    return true;
}

namespace {

// IcebergFeature's arithmetic. (Math.pow(x, 2.0) is x * x.)
i32 radius_round(FeatureRandom& random, i32 y_off, i32 height, i32 width) {
    const float k = 3.5f - random.next_float();
    float scale =
        (1.0f - static_cast<float>(static_cast<double>(y_off) * static_cast<double>(y_off)) / (static_cast<float>(height) * k)) *
        static_cast<float>(width);
    if (height > 15 + random.next_int(5)) {
        const i32 temp_y_off = y_off < 3 + random.next_int(6) ? y_off / 2 : y_off;
        scale = (1.0f - static_cast<float>(temp_y_off) / (static_cast<float>(height) * k * 0.4f)) * static_cast<float>(width);
    }
    return ceil_int(scale / 2.0f);
}
i32 radius_ellipse(i32 y_off, i32 height, i32 width) {
    const float scale = (1.0f - static_cast<float>(static_cast<double>(y_off) * static_cast<double>(y_off)) /
                                    (static_cast<float>(height) * 1.0f)) *
                        static_cast<float>(width);
    return ceil_int(scale / 2.0f);
}
i32 radius_steep(FeatureRandom& random, i32 y_off, i32 height, i32 width) {
    const float k = 1.0f + random.next_float() / 2.0f;
    const float scale = (1.0f - static_cast<float>(y_off) / (static_cast<float>(height) * k)) * static_cast<float>(width);
    return ceil_int(scale / 2.0f);
}
// signedDistanceEllipse, with the cosine and sine of its angle.
double distance_ellipse(i32 xo, i32 zo, i32 origin_x, i32 origin_z, i32 a, i32 c, double cosine, double sine) {
    const double along =
        (static_cast<double>(xo - origin_x) * cosine - static_cast<double>(zo - origin_z) * sine) / static_cast<double>(a);
    const double across =
        (static_cast<double>(xo - origin_x) * sine + static_cast<double>(zo - origin_z) * cosine) / static_cast<double>(c);
    return along * along + across * across - 1.0;
}
bool is_iceberg(u16 state) { return state == blk::kPackedIce || state == blk::kSnowBlock || state == blk::kBlueIce; }

}  // namespace

// IcebergFeature: a round or a long iceberg at the sea's level - a cone above the water, a blunter
// one below it, smoothed, and sometimes with a bay cut out of it.
bool Decorator::place_iceberg(u16 main, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    origin.y = settings::kSeaLevel;
    const bool snow_on_top = random.next_double() > 0.7;
    const double shape_angle = random.next_double() * 2.0 * 3.141592653589793;
    const i32 shape_ellipse_a = 11 - random.next_int(5);
    const i32 shape_ellipse_c = 3 + random.next_int(3);
    const bool is_ellipse = random.next_double() > 0.7;
    i32 over_water_height = is_ellipse ? random.next_int(6) + 6 : random.next_int(15) + 3;
    if (!is_ellipse && random.next_double() > 0.9) over_water_height += random.next_int(19) + 7;
    const i32 under_water_height = std::min(over_water_height + random.next_int(11), 18);
    const i32 wider = random.next_int(7);
    const i32 width = std::min(over_water_height + wider - random.next_int(5), 11);
    const i32 a = is_ellipse ? shape_ellipse_a : 11;
    // (An angle that comes from a double: our own cosine, which is the JVM's but for the last bit
    // of one angle in nine hundred - jmath.hpp.)
    const double cosine = jmath::cos(shape_angle), sine = jmath::sin(shape_angle);

    // setIcebergBlock
    const auto set_block = [&](Pos at, i32 h_diff, i32 height) {
        const u16 state = region.block(at.x, at.y, at.z);
        if (!world::is_air(state) && state != blk::kSnowBlock && state != blk::kIce && !is_water(state)) return;
        const bool randomness = !is_ellipse || random.next_double() > 0.05;
        const i32 divisor = is_ellipse ? 3 : 2;
        if (snow_on_top && !is_water(state) &&
            !(static_cast<double>(h_diff) >
              static_cast<double>(random.next_int(std::max(1, height / divisor))) + static_cast<double>(height) * 0.6) &&
            randomness)
            region.set_block(at.x, at.y, at.z, blk::kSnowBlock, 3);
        else
            region.set_block(at.x, at.y, at.z, main, 3);
    };
    // generateIcebergBlock
    const auto generate = [&](i32 height, i32 xo, i32 y_off, i32 zo, i32 radius, i32 ellipse_a) {
        double signed_dist;
        if (is_ellipse) {
            // getEllipseC
            i32 c = shape_ellipse_c;
            if (y_off > 0 && height - y_off <= 3) c = c - (4 - (height - y_off));
            signed_dist = distance_ellipse(xo, zo, 0, 0, ellipse_a, c, cosine, sine);
        } else {
            // signedDistanceCircle
            const float off = 10.0f * std::clamp(random.next_float(), 0.2f, 0.8f) / static_cast<float>(radius);
            signed_dist = static_cast<double>(off) + static_cast<double>(xo) * static_cast<double>(xo) +
                          static_cast<double>(zo) * static_cast<double>(zo) -
                          static_cast<double>(radius) * static_cast<double>(radius);
        }
        if (!(signed_dist < 0.0)) return;
        const double compare = is_ellipse ? -0.5 : static_cast<double>(-6 - random.next_int(3));
        if (signed_dist > compare && random.next_double() > 0.9) return;
        set_block({origin.x + xo, origin.y + y_off, origin.z + zo}, height - y_off, height);
    };

    for (i32 xo = -a; xo < a; ++xo)
        for (i32 zo = -a; zo < a; ++zo)
            for (i32 y_off = 0; y_off < over_water_height; ++y_off) {
                const i32 radius = is_ellipse ? radius_ellipse(y_off, over_water_height, width)
                                              : radius_round(random, y_off, over_water_height, width);
                if (is_ellipse || xo < radius) generate(over_water_height, xo, y_off, zo, radius, a);
            }

    // smooth: what hangs in the air goes, and what sticks out on three sides.
    {
        const i32 reach = is_ellipse ? shape_ellipse_a : width / 2;
        for (i32 x = -reach; x <= reach; ++x)
            for (i32 z = -reach; z <= reach; ++z)
                for (i32 y_off = 0; y_off <= over_water_height; ++y_off) {
                    const Pos at{origin.x + x, origin.y + y_off, origin.z + z};
                    const u16 state = region.block(at.x, at.y, at.z);
                    if (!is_iceberg(state) && !is_snow_layer(state)) continue;
                    if (world::is_air(region.block(at.x, at.y - 1, at.z))) {
                        region.set_block(at.x, at.y, at.z, world::kAir, 3);
                        region.set_block(at.x, at.y + 1, at.z, world::kAir, 3);
                    } else if (is_iceberg(state)) {
                        const i32 open =
                            !is_iceberg(region.block(at.x - 1, at.y, at.z)) + !is_iceberg(region.block(at.x + 1, at.y, at.z)) +
                            !is_iceberg(region.block(at.x, at.y, at.z - 1)) + !is_iceberg(region.block(at.x, at.y, at.z + 1));
                        if (open >= 3) region.set_block(at.x, at.y, at.z, world::kAir, 3);
                    }
                }
    }

    for (i32 xo = -a; xo < a; ++xo)
        for (i32 zo = -a; zo < a; ++zo)
            for (i32 y_off = -1; y_off > -under_water_height; --y_off) {
                const i32 new_a =
                    is_ellipse ? ceil_int(static_cast<float>(a) *
                                          (1.0f - static_cast<float>(static_cast<double>(y_off) * static_cast<double>(y_off)) /
                                                      (static_cast<float>(under_water_height) * 8.0f)))
                               : a;
                const i32 radius = radius_steep(random, -y_off, under_water_height, width);
                if (xo < radius) generate(under_water_height, xo, y_off, zo, radius, new_a);
            }

    const bool do_cut_out = is_ellipse ? random.next_double() > 0.1 : random.next_double() > 0.7;
    if (!do_cut_out) return true;

    // generateCutOut
    const i32 sign_x = random.next_boolean() ? -1 : 1;
    const i32 sign_z = random.next_boolean() ? -1 : 1;
    const i32 height = over_water_height;
    i32 x_off = random.next_int(std::max(width / 2 - 2, 1));
    if (random.next_boolean()) x_off = width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
    i32 z_off = random.next_int(std::max(width / 2 - 2, 1));
    if (random.next_boolean()) z_off = width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
    if (is_ellipse) x_off = z_off = random.next_int(std::max(shape_ellipse_a - 5, 1));  // (one draw for the two)
    const i32 local_x = sign_x * x_off, local_z = sign_z * z_off;
    const double angle = is_ellipse ? shape_angle + 1.5707963267948966 : random.next_double() * 2.0 * 3.141592653589793;
    const double cut_cosine = jmath::cos(angle), cut_sine = jmath::sin(angle);
    const auto carve = [&](i32 radius, i32 y_off, bool under_water) {
        const i32 ca = radius + 1 + shape_ellipse_a / 3;
        const i32 cc = std::min(radius - 3, 3) + shape_ellipse_c / 2 - 1;
        for (i32 xo = -ca; xo < ca; ++xo)
            for (i32 zo = -ca; zo < ca; ++zo) {
                if (!(distance_ellipse(xo, zo, local_x, local_z, ca, cc, cut_cosine, cut_sine) < 0.0)) continue;
                const Pos at{origin.x + xo, origin.y + y_off, origin.z + zo};
                const u16 state = region.block(at.x, at.y, at.z);
                if (!is_iceberg(state) && state != blk::kSnowBlock) continue;
                if (under_water) {
                    region.set_block(at.x, at.y, at.z, blk::kWater, 3);
                } else {
                    region.set_block(at.x, at.y, at.z, world::kAir, 3);
                    // removeFloatingSnowLayer
                    if (is_snow_layer(region.block(at.x, at.y + 1, at.z))) region.set_block(at.x, at.y + 1, at.z, world::kAir, 3);
                }
            }
    };
    for (i32 y_off = 0; y_off < height - 3; ++y_off) {
        const i32 radius = radius_round(random, y_off, height, width);
        carve(radius, y_off, false);
    }
    for (i32 y_off = -1; y_off > -height + random.next_int(5); --y_off) {
        const i32 radius = radius_steep(random, -y_off, height, width);
        carve(radius, y_off, true);
    }
    return true;
}

// SnowAndFreezeFeature: ice on the water and snow on the ground of a chunk, where its biome is
// cold enough.
bool Decorator::place_freeze_top_layer(Pos origin) {
    Region& region = region_;
    for (i32 dx = 0; dx < 16; ++dx) {
        for (i32 dz = 0; dz < 16; ++dz) {
            const i32 x = origin.x + dx, z = origin.z + dz;
            const i32 y = region.height(hm::kMotionBlocking, x, z);
            const u16 biome = region.biome(x, y, z);
            if (should_freeze(region, biome, x, y - 1, z)) region.set_block(x, y - 1, z, blk::kIce, 2);
            if (!should_snow(region, biome, x, y, z)) continue;
            region.set_block(x, y, z, blk::kSnow, 2);
            // A block that shows whether snow lies on it does.
            const u16 below = region.block(x, y - 1, z);
            for (const auto& snowy : ft::kSnowy)
                if (snowy[0] == below) region.set_block(x, y - 1, z, snowy[1], 2);
        }
    }
    return true;
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
