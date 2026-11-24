// SPDX-License-Identifier: GPL-3.0-or-later
// Trees: TreeFeature and FallenTreeFeature of the 26.3 jar, with what they are made of - the trunk
// placers, the foliage placers, the two sizes, the mangrove's roots and the tree decorators
// (net.minecraft.world.level.levelgen.feature.{trunkplacers,foliageplacers,featuresize,rootplacers,
// treedecorators}). A function a class, named after it; which a tree has, and with what numbers, is
// a row of the generated tables (tools/mcgen/gen_trees.py).
//
// The order of everything is vanilla's, and more of it decides the outcome than in any other
// feature: every draw is a statement of its own, the blocks a tree set are kept in sets whose order
// is a Java HashSet's (java_hash.hpp) - decorators walk them with a draw a block - and the distance
// of a leaf from its log is whatever TreeFeature.updateLeaves leaves it at, which follows the order
// leaves come off such a set.
#include <algorithm>
#include <cmath>

#include "nbt/nbt.hpp"
#include "phys/mth.hpp"
#include "worldgen/feature.hpp"
#include "worldgen/jmath.hpp"
#include "worldgen/mth.hpp"
#include "worldgen/props.hpp"
#include "worldgen/shape.hpp"

namespace worldgen {

namespace blk = proto::gen::block;
namespace hm = world::heightmap;
using ft::Prop;

namespace {

constexpr u32 kHorizontal[4] = {world::kNorth, world::kEast, world::kSouth, world::kWest};  // Direction.Plane.HORIZONTAL
constexpr i32 kNoAxis = -1;

u32 clockwise(u32 direction) {
    switch (direction) {
        case world::kNorth: return world::kEast;
        case world::kEast: return world::kSouth;
        case world::kSouth: return world::kWest;
        default: return world::kNorth;
    }
}
Pos moved(Pos at, u32 direction, i32 by = 1) {
    return {at.x + world::kStepX[direction] * by, at.y + world::kStepY[direction] * by, at.z + world::kStepZ[direction] * by};
}
bool same(Pos a, Pos b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
i32 manhattan(Pos a, Pos b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z); }
// Direction.Plane.HORIZONTAL.getRandomDirection
u32 random_horizontal(FeatureRandom& random) { return kHorizontal[random.next_int(4)]; }

// FeatureSize.getSizeAtHeight
i32 size_at(const ft::TreeSize& size, i32 tree_height, i32 y) {
    if (y < size.limit) return size.lower_size;
    if (!size.three) return size.upper_size;
    return y >= tree_height - size.upper_limit ? size.upper_size : size.middle_size;
}

}  // namespace

// What a tree is built with. `tree` is null for a fallen one, which has decorators and no more.
struct TreeBuilder {
    Decorator& d;
    Region& region;
    FeatureWork& work;
    FeatureRandom& random;
    const ft::Tree* tree;
    bool keep_decorations = true;  // (a fallen tree's decorations are set and not remembered)

    // ----------------------------------------------------------------------------- what is asked
    u16 block(Pos p) { return region.block(p.x, p.y, p.z); }
    bool is_air(Pos p) { return world::is_air(block(p)); }
    bool valid_tree_pos(Pos p) { return in_state_set(ft::kSetTreeGrowsThrough, block(p)); }  // TreeFeature.validTreePos
    bool is_block(Pos p, u32 default_state) { return world::block_of(block(p)) == world::block_of(default_state); }

    // ------------------------------------------------------------------- the setters of TreeFeature.place
    void set_root(Pos p, u16 state) {
        work.roots.add(p.x, p.y, p.z);
        region.set_block(p.x, p.y, p.z, state, 19);
    }
    void set_trunk(Pos p, u16 state) {
        work.trunks.add(p.x, p.y, p.z);
        region.set_block(p.x, p.y, p.z, state, 19);
    }
    void set_foliage(Pos p, u16 state) {
        work.foliage.add(p.x, p.y, p.z);
        region.set_block(p.x, p.y, p.z, state, 19);
    }
    void set_decoration(Pos p, u16 state) {
        if (keep_decorations) work.decorations.add(p.x, p.y, p.z);
        region.set_block(p.x, p.y, p.z, state, 19);
    }

    // ------------------------------------------------------------------------------- TrunkPlacer
    struct Attachment {  // FoliagePlacer.FoliageAttachment
        Pos pos;
        i32 radius_offset;
        bool double_trunk;
    };
    void attach(Pos pos, i32 radius_offset, bool double_trunk) {
        work.attachments.push_back({pos.x, pos.y, pos.z, radius_offset, double_trunk});
    }

    const ft::Trunk& trunk() const { return ft::kTrunks[tree->trunk]; }
    // TrunkPlacer.validTreePos (the mangrove's trunk grows through more), isFree.
    bool trunk_valid(Pos p) {
        const u16 state = block(p);
        if (in_state_set(ft::kSetTreeGrowsThrough, state)) return true;
        return trunk().type == ft::TrunkType::kUpwardsBranching && in_state_set(trunk().set, state);
    }
    bool is_free(Pos p) { return trunk_valid(p) || in_state_set(ft::kSetLogs, block(p)); }
    void place_below_trunk(Pos p) {
        const i32 state = d.optional_state(ft::kProviders[tree->below_trunk_provider], random, p);
        if (state >= 0) set_trunk(p, static_cast<u16>(state));
    }
    // placeLog: the trunk's block, turned to an axis where the placer says so.
    bool place_log(Pos p, i32 axis = kNoAxis) {
        if (!trunk_valid(p)) return false;
        u16 state = d.state_of(ft::kProviders[tree->trunk_provider], random, p);
        if (axis != kNoAxis) state = prop::with(state, Prop::kAxis, static_cast<u32>(axis));
        set_trunk(p, state);
        return true;
    }
    void place_log_if_free(Pos p) {
        if (is_free(p)) place_log(p);
    }

    void straight_trunk(i32 height, Pos o) {
        place_below_trunk({o.x, o.y - 1, o.z});
        for (i32 y = 0; y < height; ++y) place_log({o.x, o.y + y, o.z});
        attach({o.x, o.y + height, o.z}, 0, false);
    }

    void forking_trunk(i32 height, Pos o) {
        place_below_trunk({o.x, o.y - 1, o.z});
        const u32 lean = random_horizontal(random);
        const i32 lean_height = height - random.next_int(4) - 1;
        i32 lean_steps = 3 - random.next_int(3);
        i32 tx = o.x, tz = o.z;
        bool topped = false;
        i32 top = 0;
        for (i32 yo = 0; yo < height; ++yo) {
            const i32 yy = o.y + yo;
            if (yo >= lean_height && lean_steps > 0) {
                tx += world::kStepX[lean];
                tz += world::kStepZ[lean];
                --lean_steps;
            }
            if (place_log({tx, yy, tz})) topped = true, top = yy + 1;
        }
        if (topped) attach({tx, top, tz}, 1, false);
        tx = o.x, tz = o.z;
        const u32 branch = random_horizontal(random);
        if (branch == lean) return;
        const i32 branch_pos = lean_height - random.next_int(2) - 1;
        i32 branch_steps = 1 + random.next_int(3);
        topped = false;
        for (i32 yo = branch_pos; yo < height && branch_steps > 0; ++yo, --branch_steps) {
            if (yo < 1) continue;
            const i32 yy = o.y + yo;
            tx += world::kStepX[branch];
            tz += world::kStepZ[branch];
            if (place_log({tx, yy, tz})) topped = true, top = yy + 1;
        }
        if (topped) attach({tx, top, tz}, 0, false);
    }

    void giant_trunk(i32 height, Pos o) {
        place_below_trunk({o.x, o.y - 1, o.z});
        place_below_trunk({o.x + 1, o.y - 1, o.z});
        place_below_trunk({o.x, o.y - 1, o.z + 1});
        place_below_trunk({o.x + 1, o.y - 1, o.z + 1});
        for (i32 h = 0; h < height; ++h) {
            place_log_if_free({o.x, o.y + h, o.z});
            if (h >= height - 1) continue;
            place_log_if_free({o.x + 1, o.y + h, o.z});
            place_log_if_free({o.x + 1, o.y + h, o.z + 1});
            place_log_if_free({o.x, o.y + h, o.z + 1});
        }
        attach({o.x, o.y + height, o.z}, 0, true);
    }

    void mega_jungle_trunk(i32 height, Pos o) {
        giant_trunk(height, o);
        i32 branch_height = height - 2 - random.next_int(4);
        while (branch_height > height / 2) {
            const float angle = random.next_float() * 6.2831855f;
            i32 bx = 0, bz = 0;
            for (i32 b = 0; b < 5; ++b) {
                bx = static_cast<i32>(1.5f + phys::mth_cos(static_cast<double>(angle)) * static_cast<float>(b));
                bz = static_cast<i32>(1.5f + phys::mth_sin(static_cast<double>(angle)) * static_cast<float>(b));
                place_log({o.x + bx, o.y + branch_height - 3 + b / 2, o.z + bz});
            }
            attach({o.x + bx, o.y + branch_height, o.z + bz}, -2, false);
            const i32 down = 2 + random.next_int(4);
            branch_height -= down;
        }
    }

    void dark_oak_trunk(i32 height, Pos o) {
        place_below_trunk({o.x, o.y - 1, o.z});
        place_below_trunk({o.x + 1, o.y - 1, o.z});
        place_below_trunk({o.x, o.y - 1, o.z + 1});
        place_below_trunk({o.x + 1, o.y - 1, o.z + 1});
        const u32 lean = random_horizontal(random);
        const i32 lean_height = height - random.next_int(4);
        i32 lean_steps = 2 - random.next_int(3);
        i32 tx = o.x, tz = o.z;
        const i32 top = o.y + height - 1;
        for (i32 dy = 0; dy < height; ++dy) {
            if (dy >= lean_height && lean_steps > 0) {
                tx += world::kStepX[lean];
                tz += world::kStepZ[lean];
                --lean_steps;
            }
            const Pos p{tx, o.y + dy, tz};
            if (!in_state_set(ft::kSetAirOrLeaves, block(p))) continue;  // TreeFeature.isAirOrLeaves
            place_log(p);
            place_log({p.x + 1, p.y, p.z});
            place_log({p.x, p.y, p.z + 1});
            place_log({p.x + 1, p.y, p.z + 1});
        }
        attach({tx, top, tz}, 0, true);
        for (i32 ox = -1; ox <= 2; ++ox)
            for (i32 oz = -1; oz <= 2; ++oz) {
                if (ox >= 0 && ox <= 1 && oz >= 0 && oz <= 1) continue;
                if (random.next_int(3) > 0) continue;
                const i32 length = random.next_int(3) + 2;
                for (i32 by = 0; by < length; ++by) place_log({o.x + ox, top - by - 1, o.z + oz});
                attach({o.x + ox, top, o.z + oz}, 0, false);
            }
    }

    // FancyTrunkPlacer: a trunk, and limbs out to the clusters of leaves.
    static float tree_shape(i32 height, i32 y) {
        if (static_cast<float>(y) < static_cast<float>(height) * 0.3f) return -1.0f;
        const float radius = static_cast<float>(height) / 2.0f;
        const float adjacent = radius - static_cast<float>(y);
        float distance = static_cast<float>(std::sqrt(static_cast<double>(radius * radius - adjacent * adjacent)));  // Mth.sqrt
        if (adjacent == 0.0f)
            distance = radius;
        else if (std::fabs(adjacent) >= radius)
            return 0.0f;
        return distance * 0.5f;
    }
    static bool trim_branches(i32 height, i32 local_y) {
        return static_cast<double>(local_y) >= static_cast<double>(height) * 0.2;
    }
    bool make_limb(Pos start, Pos end, bool do_place) {
        if (!do_place && same(start, end)) return true;
        const i32 dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
        const i32 steps = std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz)));
        const float fx = static_cast<float>(dx) / static_cast<float>(steps);
        const float fy = static_cast<float>(dy) / static_cast<float>(steps);
        const float fz = static_cast<float>(dz) / static_cast<float>(steps);
        for (i32 i = 0; i <= steps; ++i) {
            const Pos p{start.x + floor_int(0.5f + static_cast<float>(i) * fx),
                        start.y + floor_int(0.5f + static_cast<float>(i) * fy),
                        start.z + floor_int(0.5f + static_cast<float>(i) * fz)};
            if (do_place) {
                // getLogAxis: along whichever way it has gone further from where the limb began.
                const i32 x_far = std::abs(p.x - start.x), z_far = std::abs(p.z - start.z);
                const i32 far = std::max(x_far, z_far);
                place_log(p, far > 0 ? (x_far == far ? 0 : 2) : 1);
            } else if (!is_free(p)) {
                return false;
            }
        }
        return true;
    }
    void fancy_trunk(i32 tree_height, Pos o) {
        const i32 height = tree_height + 2;
        const i32 trunk_height = floor_int(static_cast<double>(height) * 0.618);
        place_below_trunk({o.x, o.y - 1, o.z});
        // (clustersPerY: Math.min(1, floor(1.382 + (height / 13)^2)) - one, whatever the height.)
        const i32 trunk_top = o.y + trunk_height;
        using Coords = FeatureWork::Limb;
        std::vector<Coords>& coords = work.fancy;
        coords.clear();
        i32 relative_y = height - 5;
        coords.push_back({{o.x, o.y + relative_y, o.z}, trunk_top});
        for (; relative_y >= 0; --relative_y) {
            const float shape = tree_shape(height, relative_y);
            if (shape < 0.0f) continue;
            const double radius = 1.0 * static_cast<double>(shape) * (static_cast<double>(random.next_float()) + 0.328);
            const double angle = static_cast<double>(random.next_float() * 2.0f) * 3.141592653589793;
            const double x = radius * jmath::sin(angle) + 0.5;
            const double z = radius * jmath::cos(angle) + 0.5;
            const Pos check_start{o.x + floor_int(x), o.y + relative_y - 1, o.z + floor_int(z)};
            const Pos check_end{check_start.x, check_start.y + 5, check_start.z};
            if (!make_limb(check_start, check_end, false)) continue;
            const i32 dx = o.x - check_start.x, dz = o.z - check_start.z;
            const double branch_height =
                static_cast<double>(check_start.y) - std::sqrt(static_cast<double>(dx * dx + dz * dz)) * 0.381;
            const i32 branch_top = branch_height > static_cast<double>(trunk_top) ? trunk_top : static_cast<i32>(branch_height);
            const Pos base{o.x, branch_top, o.z};
            if (make_limb(base, check_start, false)) coords.push_back({check_start, base.y});
        }
        make_limb(o, {o.x, o.y + trunk_height, o.z}, true);
        // makeBranches
        for (usize i = 0; i < coords.size(); ++i) {
            const Coords c = coords[i];
            const Pos base{o.x, c.branch_base, o.z};
            if (!same(base, c.pos) && trim_branches(height, c.branch_base - o.y)) make_limb(base, c.pos, true);
        }
        for (const Coords& c : coords)
            if (trim_branches(height, c.branch_base - o.y)) attach(c.pos, 0, false);
    }

    void bending_trunk(i32 height, Pos o) {
        const ft::Trunk& t = trunk();
        const u32 direction = random_horizontal(random);
        const i32 log_height = height - 1;
        Pos p = o;
        place_below_trunk({p.x, p.y - 1, p.z});
        for (i32 i = 0; i <= log_height; ++i) {
            const i32 bend_at = log_height + random.next_int(2);
            if (i + 1 >= bend_at) p = moved(p, direction);
            if (valid_tree_pos(p)) place_log(p);
            if (i >= t.a) attach(p, 0, false);
            ++p.y;
        }
        const i32 length = sample(ft::kInts[t.b], random);
        for (i32 i = 0; i <= length; ++i) {
            if (valid_tree_pos(p)) place_log(p);
            attach(p, 0, false);
            p = moved(p, direction);
        }
    }

    void upwards_branching_trunk(i32 height, Pos o) {
        const ft::Trunk& t = trunk();
        for (i32 hp = 0; hp < height; ++hp) {
            const i32 current = o.y + hp;
            if (place_log({o.x, current, o.z}) && hp < height - 1 && random.next_float() < t.f) {
                const u32 direction = random_horizontal(random);
                const i32 branch_length = sample(ft::kInts[t.b], random);
                const i32 shorter = sample(ft::kInts[t.b], random);
                const i32 branch_pos = std::max(0, branch_length - shorter - 1);
                i32 branch_steps = sample(ft::kInts[t.a], random);
                // placeBranch
                i32 along = current + branch_pos;
                i32 lx = o.x, lz = o.z;
                for (i32 at = branch_pos; at < height && branch_steps > 0; ++at, --branch_steps) {
                    if (at < 1) continue;
                    const i32 placement = current + at;
                    lx += world::kStepX[direction];
                    lz += world::kStepZ[direction];
                    along = placement;
                    if (place_log({lx, placement, lz})) ++along;
                    attach({lx, placement, lz}, 0, false);
                }
                if (along - current > 1) {
                    attach({lx, along, lz}, 0, false);
                    attach({lx, along - 2, lz}, 0, false);
                }
            }
            if (hp == height - 1) attach({o.x, current + 1, o.z}, 0, false);
        }
    }

    Attachment cherry_branch(i32 height, Pos o, i32 axis, u32 direction, i32 offset_from_origin, bool middle_continues) {
        const ft::Trunk& t = trunk();
        Pos log{o.x, o.y + offset_from_origin, o.z};
        const i32 end_offset = height - 1 + sample(ft::kInts[t.e], random);
        const bool extend = middle_continues || end_offset < offset_from_origin;
        const i32 distance_to_trunk = sample(ft::kInts[t.b], random) + (extend ? 1 : 0);
        const Pos end{o.x + world::kStepX[direction] * distance_to_trunk, o.y + end_offset,
                      o.z + world::kStepZ[direction] * distance_to_trunk};
        const i32 steps = extend ? 2 : 1;
        for (i32 i = 0; i < steps; ++i) {
            log = moved(log, direction);
            place_log(log, axis);
        }
        const u32 vertical = end.y > log.y ? world::kUp : world::kDown;
        for (;;) {
            const i32 distance = manhattan(log, end);
            if (distance == 0) break;
            const float chance = static_cast<float>(std::abs(end.y - log.y)) / static_cast<float>(distance);
            const bool grow_vertically = random.next_float() < chance;
            log = moved(log, grow_vertically ? vertical : direction);
            place_log(log, grow_vertically ? kNoAxis : axis);
        }
        return {{end.x, end.y + 1, end.z}, 0, false};
    }
    void cherry_trunk(i32 height, Pos o) {
        const ft::Trunk& t = trunk();
        place_below_trunk({o.x, o.y - 1, o.z});
        const i32 first = std::max(0, height - 1 + sample(ft::kInts[t.c], random));
        i32 second = std::max(0, height - 1 + sample(ft::kInts[t.d], random));
        if (second >= first) ++second;
        const i32 count = sample(ft::kInts[t.a], random);
        const bool middle = count == 3, both = count >= 2;
        const i32 trunk_height = middle ? height : both ? std::max(first, second) + 1 : first + 1;
        for (i32 y = 0; y < trunk_height; ++y) place_log({o.x, o.y + y, o.z});
        if (middle) attach({o.x, o.y + trunk_height, o.z}, 0, false);
        const u32 direction = random_horizontal(random);
        const i32 axis = static_cast<i32>(prop::axis_of(direction));
        const Attachment one = cherry_branch(height, o, axis, direction, first, first < trunk_height - 1);
        work.attachments.push_back({one.pos.x, one.pos.y, one.pos.z, one.radius_offset, one.double_trunk});
        if (both) {
            const Attachment two = cherry_branch(height, o, axis, world::opposite(direction), second, second < trunk_height - 1);
            work.attachments.push_back({two.pos.x, two.pos.y, two.pos.z, two.radius_offset, two.double_trunk});
        }
    }

    void poplar_trunk(i32 height, Pos o) {
        const ft::Trunk& t = trunk();
        place_below_trunk({o.x, o.y - 1, o.z});
        const i32 up_to_branches = height - sample(ft::kInts[t.a], random);
        for (i32 y = 0; y < height; ++y) {
            place_log({o.x, o.y + y, o.z});
            // getShuffledBranchDirections: all six shuffled (Direction.allShuffled), the level ones of them in that order.
            u32 all[6] = {world::kDown, world::kUp, world::kNorth, world::kSouth, world::kWest, world::kEast};
            for (i32 i = 6; i > 1; --i) std::swap(all[i - 1], all[random.next_int(i)]);
            if (up_to_branches - 1 != y) continue;
            u32 level[4];
            u32 found = 0;
            for (const u32 direction : all)
                if (direction >= 2) level[found++] = direction;
            const i32 branches = sample(ft::kInts[t.b], random);
            for (i32 i = 0; i < branches; ++i)
                place_log(moved({o.x, o.y + y, o.z}, level[i]), static_cast<i32>(prop::axis_of(level[i])));
        }
        attach({o.x, o.y + up_to_branches, o.z}, 0, false);
    }

    void place_trunk(i32 height, Pos origin) {
        switch (trunk().type) {
            case ft::TrunkType::kStraight: return straight_trunk(height, origin);
            case ft::TrunkType::kForking: return forking_trunk(height, origin);
            case ft::TrunkType::kGiant: return giant_trunk(height, origin);
            case ft::TrunkType::kMegaJungle: return mega_jungle_trunk(height, origin);
            case ft::TrunkType::kDarkOak: return dark_oak_trunk(height, origin);
            case ft::TrunkType::kFancy: return fancy_trunk(height, origin);
            case ft::TrunkType::kBending: return bending_trunk(height, origin);
            case ft::TrunkType::kUpwardsBranching: return upwards_branching_trunk(height, origin);
            case ft::TrunkType::kCherry: return cherry_trunk(height, origin);
            case ft::TrunkType::kPoplar: return poplar_trunk(height, origin);
        }
    }

    // ----------------------------------------------------------------------------- FoliagePlacer
    const ft::Foliage& foliage() const { return ft::kFoliages[tree->foliage]; }

    // foliageHeight, foliageRadius
    i32 foliage_height(i32 tree_height) {
        const ft::Foliage& f = foliage();
        switch (f.type) {
            case ft::FoliageType::kBlob:
            case ft::FoliageType::kBush:
            case ft::FoliageType::kFancy:
            case ft::FoliageType::kMegaJungle: return f.a;
            case ft::FoliageType::kAcacia: return 0;
            case ft::FoliageType::kDarkOak: return 4;
            case ft::FoliageType::kSpruce: return std::max(4, tree_height - sample(ft::kInts[f.a], random));
            case ft::FoliageType::kPine:
            case ft::FoliageType::kMegaPine:
            case ft::FoliageType::kRandomSpread:
            case ft::FoliageType::kCherry:
            case ft::FoliageType::kPoplar: return sample(ft::kInts[f.a], random);
        }
        return 0;
    }
    i32 foliage_radius(i32 trunk_height) {
        const ft::Foliage& f = foliage();
        const i32 radius = sample(ft::kInts[f.radius], random);
        if (f.type != ft::FoliageType::kPine) return radius;
        return radius + random.next_int(std::max(trunk_height + 1, 1));
    }

    // tryPlaceLeaf: where a tree may grow and no leaf was put by hand, the foliage's block - with the water that is there.
    bool try_place_leaf(Pos p) {
        const u16 there = block(p);
        if (prop::has(there, Prop::kPersistent) && prop::flag(there, Prop::kPersistent)) return false;
        if (!in_state_set(ft::kSetTreeGrowsThrough, there)) return false;
        u16 state = d.state_of(ft::kProviders[tree->foliage_provider], random, p);
        if (prop::has(state, Prop::kWaterlogged))
            state = prop::with_flag(state, Prop::kWaterlogged, in_state_set(ft::kSetWaterSource, block(p)));
        set_foliage(p, state);
        return true;
    }

    // shouldSkipLocation: by how far a block is from the middle of its row.
    bool skip(i32 dx, i32 y, i32 dz, i32 radius, bool double_trunk) {
        const ft::Foliage& f = foliage();
        switch (f.type) {
            case ft::FoliageType::kBlob: return dx == radius && dz == radius && (random.next_int(2) == 0 || y == 0);
            case ft::FoliageType::kBush: return dx == radius && dz == radius && random.next_int(2) == 0;
            case ft::FoliageType::kFancy: {
                const float fx = static_cast<float>(dx) + 0.5f, fz = static_cast<float>(dz) + 0.5f;
                return fx * fx + fz * fz > static_cast<float>(radius * radius);
            }
            case ft::FoliageType::kAcacia:
                if (y == 0) return (dx > 1 || dz > 1) && dx != 0 && dz != 0;
                return dx == radius && dz == radius && radius > 0;
            case ft::FoliageType::kDarkOak:
                if (y == -1 && !double_trunk) return dx == radius && dz == radius;
                if (y == 1) return dx + dz > radius * 2 - 2;
                return false;
            case ft::FoliageType::kMegaJungle:
            case ft::FoliageType::kMegaPine:
                if (dx + dz >= 7) return true;
                return dx * dx + dz * dz > radius * radius;
            case ft::FoliageType::kPine:
            case ft::FoliageType::kSpruce: return dx == radius && dz == radius && radius > 0;
            case ft::FoliageType::kRandomSpread: return false;
            case ft::FoliageType::kCherry: {
                if (y == -1 && (dx == radius || dz == radius) && random.next_float() < f.f[0]) return true;
                const bool corner = dx == radius && dz == radius;
                if (radius > 2) return corner || (dx + dz > radius * 2 - 2 && random.next_float() < f.f[1]);
                return corner && random.next_float() < f.f[1];
            }
            case ft::FoliageType::kPoplar: break;  // (has a row of its own)
        }
        DCHECK(false);
        return false;
    }
    // shouldSkipLocationSigned: a trunk of two by two has two middles.
    bool skip_signed(i32 dx, i32 y, i32 dz, i32 radius, bool double_trunk) {
        if (foliage().type == ft::FoliageType::kDarkOak && y == 0 && double_trunk && (dx == -radius || dx >= radius) &&
            (dz == -radius || dz >= radius))
            return true;
        if (double_trunk)
            return skip(std::min(std::abs(dx), std::abs(dx - 1)), y, std::min(std::abs(dz), std::abs(dz - 1)), radius,
                        double_trunk);
        return skip(std::abs(dx), y, std::abs(dz), radius, double_trunk);
    }
    void leaves_row(Pos origin, i32 radius, i32 y, bool double_trunk) {
        const i32 more = double_trunk ? 1 : 0;
        for (i32 dx = -radius; dx <= radius + more; ++dx)
            for (i32 dz = -radius; dz <= radius + more; ++dz)
                if (!skip_signed(dx, y, dz, radius, double_trunk)) try_place_leaf({origin.x + dx, origin.y + y, origin.z + dz});
    }
    // placeLeavesRowWithHangingLeavesBelow (the cherry's)
    bool try_place_extension(float chance, Pos log, Pos p) {
        if (manhattan(p, log) >= 7) return false;
        if (random.next_float() > chance) return false;
        return try_place_leaf(p);
    }
    void leaves_row_with_hanging(Pos origin, i32 radius, i32 y, bool double_trunk, float chance, float extension_chance) {
        leaves_row(origin, radius, y, double_trunk);
        const i32 more = double_trunk ? 1 : 0;
        const Pos log{origin.x, origin.y - 1, origin.z};
        for (const u32 along : kHorizontal) {
            const u32 to_edge = clockwise(along);
            const bool positive = to_edge == world::kEast || to_edge == world::kSouth;
            const i32 offset_to_edge = positive ? radius + more : radius;
            Pos p = moved(moved({origin.x, origin.y + y - 1, origin.z}, to_edge, offset_to_edge), along, -radius);
            for (i32 offset = -radius; offset < radius + more; ++offset) {
                if (work.foliage.contains(p.x, p.y + 1, p.z) && try_place_extension(chance, log, p))
                    try_place_extension(extension_chance, log, {p.x, p.y - 1, p.z});
                p = moved(p, along);
            }
        }
    }

    // PoplarFoliagePlacer: rows that are rhombuses with a corner cut, one way round or the other.
    static bool within_rhombus(i32 radius, i32 abs_dx, i32 abs_dz, i32 cut, i32 more) {
        return abs_dx + abs_dz <= radius * 2 - (cut + more);
    }
    static bool partial_row(i32 foliage_height, i32 y) { return foliage_height - 1 == y || foliage_height - 2 == y; }
    static i32 corner_cut(i32 dx, i32 dz, i32 radius, bool partial, bool flip) {
        const bool small = flip ? ((dx > 0 && dz > 0) || (dz < 0 && dx < 0)) : ((dx > 0 && dz < 0) || (dz > 0 && dx < 0));
        return small ? radius - 1 : partial ? radius + 1 : radius;
    }
    void poplar_row(Pos origin, i32 radius, i32 y, bool double_trunk, i32 foliage_height, bool flip) {
        const i32 more = double_trunk ? 1 : 0;
        for (i32 dx = -radius; dx <= radius + more; ++dx)
            for (i32 dz = -radius; dz <= radius + more; ++dz) {
                const bool partial = partial_row(foliage_height, y);
                const i32 cut = corner_cut(dx, dz, radius, partial, flip);
                const i32 abs_dx = std::abs(dx), abs_dz = std::abs(dz);
                if (partial && (abs_dx == radius || abs_dz == radius)) continue;
                const i32 hole = random.next_float() > foliage().f[0] ? 0 : 1;
                if (!within_rhombus(radius, abs_dx, abs_dz, cut, hole)) continue;
                try_place_leaf({origin.x + dx, origin.y + y, origin.z + dz});
            }
    }
    void poplar_logs(Pos origin, i32 radius, i32 y, bool double_trunk, i32 foliage_height, bool flip) {
        const i32 more = double_trunk ? 1 : 0;
        for (i32 dx = -radius; dx <= radius + more; ++dx)
            for (i32 dz = -radius; dz <= radius + more; ++dz) {
                const i32 abs_dz = std::abs(dz), abs_dx = std::abs(dx);
                if (!within_rhombus(radius, abs_dx, abs_dz, corner_cut(dx, dz, radius, partial_row(foliage_height, y), flip), 2))
                    continue;
                if (!((abs_dz == 0 && radius - abs_dx >= 4) || (abs_dx == 0 && radius - abs_dz >= 4))) continue;
                // tryPlaceLog: a leaf of this tree becomes a log that lies along the row.
                const Pos p{origin.x + dx, origin.y + y, origin.z + dz};
                const u16 leaf = d.state_of(ft::kProviders[tree->foliage_provider], random, p);
                if (block(p) != leaf) continue;
                const u16 log = d.state_of(ft::kProviders[tree->trunk_provider], random, p);
                set_foliage(p, prop::with(log, Prop::kAxis, abs_dz == 0 ? 0 : 2));
            }
    }

    // createFoliage: the leaves at one of the places a trunk left for them.
    void create_foliage(const Attachment& at, i32 foliage_height, i32 leaf_radius) {
        const ft::Foliage& f = foliage();
        const i32 offset = sample(ft::kInts[f.offset], random);
        const bool double_trunk = at.double_trunk;
        const i32 height_with_offset = foliage_height;  // (+ foliageHeightOffset, which no trunk placer sets)
        switch (f.type) {
            case ft::FoliageType::kBlob:
                for (i32 yo = offset; yo >= offset - height_with_offset; --yo)
                    leaves_row(at.pos, std::max(leaf_radius + at.radius_offset - 1 - yo / 2, 0), yo, double_trunk);
                return;
            case ft::FoliageType::kBush:
                for (i32 yo = offset; yo >= offset - height_with_offset; --yo)
                    leaves_row(at.pos, leaf_radius + at.radius_offset - 1 - yo, yo, double_trunk);
                return;
            case ft::FoliageType::kFancy:
                for (i32 yo = offset; yo >= offset - foliage_height; --yo)
                    leaves_row(at.pos, leaf_radius + (yo == offset || yo == offset - foliage_height ? 0 : 1), yo, double_trunk);
                return;
            case ft::FoliageType::kAcacia: {
                const Pos p{at.pos.x, at.pos.y + offset, at.pos.z};
                leaves_row(p, leaf_radius + at.radius_offset, -1 - height_with_offset, double_trunk);
                leaves_row(p, leaf_radius - 1, -height_with_offset, double_trunk);
                leaves_row(p, leaf_radius + at.radius_offset - 1, 0, double_trunk);
                return;
            }
            case ft::FoliageType::kDarkOak: {
                const Pos p{at.pos.x, at.pos.y + offset, at.pos.z};
                if (double_trunk) {
                    leaves_row(p, leaf_radius + 2, -1, double_trunk);
                    leaves_row(p, leaf_radius + 3, 0, double_trunk);
                    leaves_row(p, leaf_radius + 2, 1, double_trunk);
                    if (random.next_boolean()) leaves_row(p, leaf_radius, 2, double_trunk);
                } else {
                    leaves_row(p, leaf_radius + 2, -1, double_trunk);
                    leaves_row(p, leaf_radius + 1, 0, double_trunk);
                }
                return;
            }
            case ft::FoliageType::kMegaJungle: {
                const i32 leaf_height = double_trunk ? foliage_height : 1 + random.next_int(2);
                for (i32 yo = offset; yo >= offset - leaf_height; --yo)
                    leaves_row(at.pos, leaf_radius + at.radius_offset + 1 - yo, yo, double_trunk);
                return;
            }
            case ft::FoliageType::kMegaPine: {
                i32 previous = 0;
                for (i32 yy = at.pos.y - height_with_offset + offset; yy <= at.pos.y + offset; ++yy) {
                    const i32 yo = at.pos.y - yy;
                    const i32 smooth = leaf_radius + at.radius_offset +
                                       floor_int(static_cast<float>(yo) / static_cast<float>(height_with_offset) * 3.5f);
                    const i32 jagged = yo > 0 && smooth == previous && (yy & 1) == 0 ? smooth + 1 : smooth;
                    leaves_row({at.pos.x, yy, at.pos.z}, jagged, 0, double_trunk);
                    previous = smooth;
                }
                return;
            }
            case ft::FoliageType::kPine: {
                i32 radius = 0;
                for (i32 yo = offset; yo >= offset - height_with_offset; --yo) {
                    leaves_row(at.pos, radius, yo, double_trunk);
                    if (radius >= 1 && yo == offset - height_with_offset + 1)
                        --radius;
                    else if (radius < leaf_radius + at.radius_offset)
                        ++radius;
                }
                return;
            }
            case ft::FoliageType::kSpruce: {
                i32 radius = random.next_int(2);
                i32 max_radius = 1, min_radius = 0;
                for (i32 yo = offset; yo >= -height_with_offset; --yo) {
                    leaves_row(at.pos, radius, yo, double_trunk);
                    if (radius >= max_radius) {
                        radius = min_radius;
                        min_radius = 1;
                        max_radius = std::min(max_radius + 1, leaf_radius + at.radius_offset);
                    } else {
                        ++radius;
                    }
                }
                return;
            }
            case ft::FoliageType::kRandomSpread:
                for (i32 i = 0; i < f.b; ++i) {
                    // (Six draws, in the order they stand in vanilla's one expression.)
                    const i32 x0 = random.next_int(leaf_radius);
                    const i32 x1 = random.next_int(leaf_radius);
                    const i32 y0 = random.next_int(foliage_height);
                    const i32 y1 = random.next_int(foliage_height);
                    const i32 z0 = random.next_int(leaf_radius);
                    const i32 z1 = random.next_int(leaf_radius);
                    try_place_leaf({at.pos.x + x0 - x1, at.pos.y + y0 - y1, at.pos.z + z0 - z1});
                }
                return;
            case ft::FoliageType::kCherry: {
                const Pos p{at.pos.x, at.pos.y + offset, at.pos.z};
                const i32 radius = leaf_radius + at.radius_offset - 1;
                leaves_row(p, radius - 2, height_with_offset - 3, double_trunk);
                leaves_row(p, radius - 1, height_with_offset - 4, double_trunk);
                for (i32 y = height_with_offset - 5; y >= 0; --y) leaves_row(p, radius, y, double_trunk);
                leaves_row_with_hanging(p, radius, -1, double_trunk, f.f[2], f.f[3]);
                leaves_row_with_hanging(p, radius - 1, -2, double_trunk, f.f[2], f.f[3]);
                return;
            }
            case ft::FoliageType::kPoplar: {
                const Pos p{at.pos.x, at.pos.y + offset, at.pos.z};
                const i32 radius = leaf_radius + at.radius_offset - 1;
                const bool flip = random.next_boolean();
                const i32 h = height_with_offset;
                poplar_row(p, radius - 2, h - 1, double_trunk, h, flip);
                poplar_row(p, radius - 1, h - 2, double_trunk, h, flip);
                poplar_row(p, radius - 1, h - 3, double_trunk, h, flip);
                for (i32 y = h - 4; y >= 1; --y) poplar_row(p, radius, y, double_trunk, h, flip);
                poplar_logs(p, radius, h - 4, double_trunk, h, flip);
                poplar_row(p, radius - 1, 0, double_trunk, h, flip);
                poplar_row(p, std::min(std::max(radius - 2, 1), 2), -1, double_trunk, h, flip);
                return;
            }
        }
    }

    // ------------------------------------------------------------------------ MangroveRootPlacer
    const ft::Roots& roots() const { return ft::kRoots[tree->roots - 1]; }
    bool can_place_root(Pos p) {
        const u16 state = block(p);
        return in_state_set(ft::kSetTreeGrowsThrough, state) || in_state_set(roots().can_grow_through, state);
    }
    // getPotentiallyWaterloggedState
    u16 with_water_of(Pos p, u16 state) {
        if (!prop::has(state, Prop::kWaterlogged)) return state;
        return prop::with_flag(state, Prop::kWaterlogged, in_state_set(ft::kSetHoldsWater, block(p)));
    }
    void place_root(Pos p) {
        const ft::Roots& r = roots();
        if (in_state_set(r.muddy_roots_in, block(p))) {
            const u16 muddy = d.state_of(ft::kProviders[r.muddy_roots_provider], random, p);
            set_root(p, with_water_of(p, muddy));
            return;
        }
        if (!can_place_root(p)) return;
        const u16 root = d.state_of(ft::kProviders[r.root_provider], random, p);
        set_root(p, with_water_of(p, root));
        if (!r.has_above) return;
        const Pos above{p.x, p.y + 1, p.z};
        if (random.next_float() < r.above_chance && is_air(above)) {
            const u16 over = d.state_of(ft::kProviders[r.above_provider], random, above);
            set_root(above, with_water_of(above, over));
        }
    }
    // potentialRootPositions: where a root goes on from a block - down, outwards, or both.
    u32 next_roots(Pos p, u32 direction, Pos root_origin, Pos out[2]) {
        const ft::Roots& r = roots();
        const Pos below{p.x, p.y - 1, p.z};
        const Pos next_to = moved(p, direction);
        const i32 width = manhattan(p, root_origin);
        out[0] = below;
        if (width > r.max_root_width - 3 && width <= r.max_root_width) {
            if (random.next_float() < r.random_skew_chance) {
                out[1] = {next_to.x, next_to.y - 1, next_to.z};
                return 2;
            }
            return 1;
        }
        if (width > r.max_root_width) return 1;
        if (random.next_float() < r.random_skew_chance) return 1;
        if (random.next_boolean()) out[0] = next_to;
        return 1;
    }
    bool simulate_roots(Pos p, u32 direction, Pos root_origin, std::vector<Pos>& found, i32 layer) {
        const i32 longest = roots().max_root_length;
        if (layer == longest || static_cast<i32>(found.size()) > longest) return false;
        Pos next[2];
        const u32 count = next_roots(p, direction, root_origin, next);
        for (u32 i = 0; i < count; ++i) {
            if (!can_place_root(next[i])) continue;
            found.push_back(next[i]);
            if (!simulate_roots(next[i], direction, root_origin, found, layer + 1)) return false;
        }
        return true;
    }
    bool place_roots(Pos origin, Pos trunk_origin) {
        std::vector<Pos>& all = work.root_list;
        std::vector<Pos>& in_direction = work.list;
        all.clear();
        for (Pos column = origin; column.y < trunk_origin.y; ++column.y)
            if (!can_place_root(column)) return false;
        all.push_back({trunk_origin.x, trunk_origin.y - 1, trunk_origin.z});
        for (const u32 direction : kHorizontal) {
            const Pos p = moved(trunk_origin, direction);
            in_direction.clear();
            if (!simulate_roots(p, direction, trunk_origin, in_direction, 0)) return false;
            all.insert(all.end(), in_direction.begin(), in_direction.end());
            all.push_back(p);
        }
        for (usize i = 0; i < all.size(); ++i) place_root(all[i]);
        return true;
    }

    // -------------------------------------------------------------------------------- TreeFeature
    i32 max_free_tree_height(i32 max_height, Pos p) {
        const ft::TreeSize& size = ft::kTreeSizes[tree->size];
        for (i32 y = 0; y <= max_height + 1; ++y) {
            const i32 r = size_at(size, max_height, y);
            for (i32 x = -r; x <= r; ++x)
                for (i32 z = -r; z <= r; ++z) {
                    const Pos at{p.x + x, p.y + y, p.z + z};
                    if (!is_free(at) || (!tree->ignore_vines && is_block(at, blk::kVine))) return y - 2;
                }
        }
        return max_height;
    }
    bool do_place(Pos origin) {
        const ft::Trunk& t = trunk();
        // TrunkPlacer.getTreeHeight
        const i32 first = random.next_int(t.height_rand_a + 1);
        const i32 second = random.next_int(t.height_rand_b + 1);
        const i32 tree_height = t.base_height + first + second;
        const i32 leaves_height = foliage_height(tree_height);
        const i32 trunk_height = tree_height - leaves_height;
        const i32 leaf_radius = foliage_radius(trunk_height);
        Pos trunk_origin = origin;
        if (tree->roots) trunk_origin.y += sample(ft::kInts[roots().trunk_offset_y], random);
        const i32 min_y = std::min(origin.y, trunk_origin.y);
        const i32 max_y = std::max(origin.y, trunk_origin.y) + tree_height + 1;
        if (min_y < Region::kMinY + 1 || max_y > Region::kMinY + Region::kHeight) return false;
        const i32 min_clipped = ft::kTreeSizes[tree->size].min_clipped_height;
        const i32 clipped = max_free_tree_height(tree_height, trunk_origin);
        if (clipped < tree_height && (min_clipped < 0 || clipped < min_clipped)) return false;
        if (tree->roots && !place_roots(origin, trunk_origin)) return false;
        work.attachments.clear();
        place_trunk(clipped, trunk_origin);
        for (usize i = 0; i < work.attachments.size(); ++i) {
            const FeatureWork::Attachment a = work.attachments[i];
            create_foliage({{a.x, a.y, a.z}, a.radius_offset, a.double_trunk}, leaves_height, leaf_radius);
        }
        return true;
    }

    // ------------------------------------------------------------------------------ TreeDecorator
    // TreeDecorator.Context: the tree's blocks as lists - a set's order, then by height (a sort that keeps the order of equals).
    static void listed(const JavaHashSet& set, std::vector<Pos>& out) {
        out.clear();
        set.each([&](i32 x, i32 y, i32 z) { out.push_back({x, y, z}); });
        std::stable_sort(out.begin(), out.end(), [](const Pos& a, const Pos& b) { return a.y < b.y; });
    }
    void shuffle(std::vector<Pos>& list) {  // Util.shuffle
        for (i32 i = static_cast<i32>(list.size()); i > 1; --i)
            std::swap(list[static_cast<usize>(i - 1)], list[static_cast<usize>(random.next_int(i))]);
    }
    void place_vine(Pos p, u16 vine, Prop side) { set_decoration(p, prop::with_flag(vine, side, true)); }
    // TreeFeature.getLowestTrunkOrRootOfTree
    void lowest_trunk_or_root(std::vector<Pos>& out) {
        const std::vector<Pos>& logs = work.logs;
        const std::vector<Pos>& root_list = work.root_list;
        out.clear();
        if (root_list.empty()) {
            out = logs;
        } else if (!logs.empty() && root_list[0].y == logs[0].y) {
            out = logs;
            out.insert(out.end(), root_list.begin(), root_list.end());
        } else {
            out = root_list;
        }
    }

    void beehive(const ft::TreeDecorator& dec) {
        const std::vector<Pos>& logs = work.logs;
        const std::vector<Pos>& leaves = work.leaves;
        if (logs.empty()) return;
        if (random.next_float() >= dec.f[0]) return;
        i32 hive_y;
        if (!leaves.empty()) {
            hive_y = std::max(leaves[0].y - 1, logs[0].y + 1);
        } else {
            const i32 up = random.next_int(3);
            hive_y = std::min(logs[0].y + 1 + up, logs.back().y);
        }
        // Beside the logs of that height, on every side but the north (SPAWN_DIRECTIONS).
        std::vector<Pos>& places = work.list;
        places.clear();
        for (const Pos& log : logs) {
            if (log.y != hive_y) continue;
            for (const u32 direction : {world::kEast, world::kSouth, world::kWest}) places.push_back(moved(log, direction));
        }
        if (places.empty()) return;
        shuffle(places);
        for (const Pos& p : places) {
            if (!is_air(p) || !is_air({p.x, p.y, p.z + 1})) continue;
            const bool placed = region.set_block(p.x, p.y, p.z, static_cast<u16>(dec.a), 19);
            if (keep_decorations) work.decorations.add(p.x, p.y, p.z);
            if (placed) {
                // BeehiveBlockEntity.storeBee: two or three bees, each so long in the hive already - and the
                // nest as a chunk that is still being made saves it (BlockEntity.saveWithFullMetadata).
                const i32 bees = 2 + random.next_int(2);
                base::ByteBuf& tag = work.tag;
                tag.len = 0;
                nbt::Writer w(tag);
                w.root(nbt::kCompound);
                w.begin_list("bees", nbt::kCompound, static_cast<u32>(bees));
                for (i32 i = 0; i < bees; ++i) {
                    const i32 ticks_in_hive = random.next_int(599);
                    w.begin_compound();
                    w.begin_compound("entity_data");
                    w.put_string("id", "minecraft:bee");
                    w.end_compound();
                    w.put_int("min_ticks_in_hive", 600);
                    w.put_int("ticks_in_hive", ticks_in_hive);
                    w.end_compound();
                }
                w.begin_compound("components");
                w.end_compound();
                w.put_string("id", "minecraft:beehive");
                w.put_int("x", p.x);
                w.put_int("y", p.y);
                w.put_int("z", p.z);
                w.end_compound();
                region.set_block_entity_tag(p.x, p.y, p.z, {tag.data, tag.len});
            }
            return;
        }
    }

    void cocoa(const ft::TreeDecorator& dec) {
        if (random.next_float() >= dec.f[0]) return;
        const std::vector<Pos>& logs = work.logs;
        if (logs.empty()) return;
        const i32 tree_y = logs[0].y;
        for (const Pos& log : logs) {
            if (log.y - tree_y > 2) continue;
            for (const u32 direction : kHorizontal) {
                if (random.next_float() > 0.25f) continue;
                const u32 opposite = world::opposite(direction);
                const Pos p{log.x + world::kStepX[opposite], log.y, log.z + world::kStepZ[opposite]};
                if (!is_air(p)) continue;
                const u32 age = static_cast<u32>(random.next_int(3));
                set_decoration(
                    p, prop::with(prop::with(dec.a, Prop::kAge2, age), Prop::kHorizontalFacing, prop::facing_of(direction)));
            }
        }
    }

    void hanging_vine(Pos p, u16 vine, Prop side) {
        place_vine(p, vine, side);
        i32 more = 4;
        --p.y;
        while (is_air(p) && more > 0) {
            place_vine(p, vine, side);
            --p.y;
            --more;
        }
    }
    void leave_vine(const ft::TreeDecorator& dec) {
        const u16 vine = static_cast<u16>(dec.a);
        for (usize i = 0; i < work.leaves.size(); ++i) {
            const Pos leaf = work.leaves[i];
            if (random.next_float() < dec.f[0] && is_air({leaf.x - 1, leaf.y, leaf.z}))
                hanging_vine({leaf.x - 1, leaf.y, leaf.z}, vine, Prop::kEast);
            if (random.next_float() < dec.f[0] && is_air({leaf.x + 1, leaf.y, leaf.z}))
                hanging_vine({leaf.x + 1, leaf.y, leaf.z}, vine, Prop::kWest);
            if (random.next_float() < dec.f[0] && is_air({leaf.x, leaf.y, leaf.z - 1}))
                hanging_vine({leaf.x, leaf.y, leaf.z - 1}, vine, Prop::kSouth);
            if (random.next_float() < dec.f[0] && is_air({leaf.x, leaf.y, leaf.z + 1}))
                hanging_vine({leaf.x, leaf.y, leaf.z + 1}, vine, Prop::kNorth);
        }
    }
    void trunk_vine(const ft::TreeDecorator& dec) {
        const u16 vine = static_cast<u16>(dec.a);
        for (usize i = 0; i < work.logs.size(); ++i) {
            const Pos log = work.logs[i];
            if (random.next_int(3) > 0 && is_air({log.x - 1, log.y, log.z}))
                place_vine({log.x - 1, log.y, log.z}, vine, Prop::kEast);
            if (random.next_int(3) > 0 && is_air({log.x + 1, log.y, log.z}))
                place_vine({log.x + 1, log.y, log.z}, vine, Prop::kWest);
            if (random.next_int(3) > 0 && is_air({log.x, log.y, log.z - 1}))
                place_vine({log.x, log.y, log.z - 1}, vine, Prop::kSouth);
            if (random.next_int(3) > 0 && is_air({log.x, log.y, log.z + 1}))
                place_vine({log.x, log.y, log.z + 1}, vine, Prop::kNorth);
        }
    }

    // AlterGroundDecorator: the ground around the foot of a tree becomes what the provider says (podzol).
    void alter_block(const ft::TreeDecorator& dec, Pos p) {
        for (i32 dy = 2; dy >= -3; --dy) {
            const Pos cursor{p.x, p.y + dy, p.z};
            const i32 with = d.optional_state(ft::kProviders[dec.provider], random, cursor);
            if (with >= 0) {
                set_decoration(cursor, static_cast<u16>(with));
                return;
            }
            if (!is_air(cursor) && dy < 0) return;
        }
    }
    void alter_circle(const ft::TreeDecorator& dec, Pos p) {
        for (i32 xx = -2; xx <= 2; ++xx)
            for (i32 zz = -2; zz <= 2; ++zz)
                if (std::abs(xx) != 2 || std::abs(zz) != 2) alter_block(dec, {p.x + xx, p.y, p.z + zz});
    }
    void alter_ground(const ft::TreeDecorator& dec) {
        std::vector<Pos>& at = work.list;
        lowest_trunk_or_root(at);
        if (at.empty()) return;
        const i32 min_y = at[0].y;
        for (usize i = 0; i < at.size(); ++i) {
            const Pos p = at[i];
            if (p.y != min_y) continue;
            alter_circle(dec, {p.x - 1, p.y, p.z - 1});
            alter_circle(dec, {p.x + 2, p.y, p.z - 1});
            alter_circle(dec, {p.x - 1, p.y, p.z + 2});
            alter_circle(dec, {p.x + 2, p.y, p.z + 2});
            for (i32 n = 0; n < 5; ++n) {
                const i32 placement = random.next_int(64);
                const i32 xx = placement % 8, zz = placement / 8;
                if (xx == 0 || xx == 7 || zz == 0 || zz == 7) alter_circle(dec, {p.x - 3 + xx, p.y, p.z - 3 + zz});
            }
        }
    }

    static u32 direction_count(u32 packed) { return packed >> 24; }
    static u32 direction_at(u32 packed, u32 i) { return packed >> (3 * i) & 7; }

    void attached_to_leaves(const ft::TreeDecorator& dec) {
        JavaHashSet& taken = work.blacklist;
        taken.clear();
        std::vector<Pos>& leaves = work.list;
        leaves = work.leaves;  // Util.shuffledCopy
        shuffle(leaves);
        for (usize i = 0; i < leaves.size(); ++i) {
            const Pos leaf = leaves[i];
            const u32 direction =
                direction_at(dec.d, static_cast<u32>(random.next_int(static_cast<i32>(direction_count(dec.d)))));
            const Pos p = moved(leaf, direction);
            if (taken.contains(p.x, p.y, p.z)) continue;
            if (random.next_float() >= dec.f[0]) continue;
            bool empty = true;  // hasRequiredEmptyBlocks
            for (i32 n = 1; n <= dec.c && empty; ++n) empty = is_air(moved(leaf, direction, n));
            if (!empty) continue;
            for (i32 z = p.z - dec.a; z <= p.z + dec.a; ++z)
                for (i32 y = p.y - dec.b; y <= p.y + dec.b; ++y)
                    for (i32 x = p.x - dec.a; x <= p.x + dec.a; ++x) taken.add(x, y, z);
            set_decoration(p, d.state_of(ft::kProviders[dec.provider], random, p));
        }
    }
    void attached_to_logs(const ft::TreeDecorator& dec) {
        std::vector<Pos>& logs = work.list;
        logs = work.logs;
        shuffle(logs);
        for (usize i = 0; i < logs.size(); ++i) {
            const u32 direction =
                direction_at(dec.d, static_cast<u32>(random.next_int(static_cast<i32>(direction_count(dec.d)))));
            const Pos p = moved(logs[i], direction);
            if (random.next_float() > dec.f[0]) continue;
            if (!is_air(p)) continue;
            set_decoration(p, d.state_of(ft::kProviders[dec.provider], random, p));
        }
    }

    void place_on_ground(const ft::TreeDecorator& dec) {
        std::vector<Pos>& at = work.list;
        lowest_trunk_or_root(at);
        if (at.empty()) return;
        const i32 min_y = at[0].y;
        i32 min_x = at[0].x, max_x = at[0].x, min_z = at[0].z, max_z = at[0].z;
        for (const Pos& p : at) {
            if (p.y != min_y) continue;
            min_x = std::min(min_x, p.x), max_x = std::max(max_x, p.x);
            min_z = std::min(min_z, p.z), max_z = std::max(max_z, p.z);
        }
        min_x -= dec.b, max_x += dec.b, min_z -= dec.b, max_z += dec.b;
        const i32 low = min_y - dec.c, high = min_y + dec.c;
        for (i32 i = 0; i < dec.a; ++i) {
            // RandomSource.nextIntBetweenInclusive, thrice
            const i32 x = random.next_int(max_x - min_x + 1) + min_x;
            const i32 y = random.next_int(high - low + 1) + low;
            const i32 z = random.next_int(max_z - min_z + 1) + min_z;
            const Pos above{x, y + 1, z};
            if (!(is_air(above) || is_block(above, blk::kVine))) continue;
            if (!in_state_set(ft::kSetSolidRender, block({x, y, z}))) continue;
            if (region.height(hm::kMotionBlockingNoLeaves, x, z) > above.y) continue;
            set_decoration(above, d.state_of(ft::kProviders[dec.provider], random, above));
        }
    }

    // ShelfMushroomDecorator
    bool has_shelf(Pos p, u16 shelf) { return is_block(p, shelf); }
    bool has_shelf_beside(Pos p, u16 shelf) {
        for (const u32 direction : kHorizontal)
            if (has_shelf(moved(p, direction), shelf)) return true;
        return false;
    }
    bool shelf_may_replace(Pos p) {
        if (!in_state_set(ft::kSetReplaceable, block(p))) return false;
        // isWaterOrWaterNearby
        return !(is_block(p, blk::kWater) || is_block({p.x + 1, p.y, p.z}, blk::kWater) ||
                 is_block({p.x - 1, p.y, p.z}, blk::kWater) || is_block({p.x, p.y, p.z - 1}, blk::kWater) ||
                 is_block({p.x, p.y, p.z + 1}, blk::kWater));
    }
    void place_shelf(Pos p, u32 facing, u16 shelf) {
        const u32 age = static_cast<u32>(random.next_int(2));
        set_decoration(p, prop::with(prop::with(shelf, Prop::kAge1, age), Prop::kHorizontalFacing, prop::facing_of(facing)));
    }
    void shelf_mushroom(const ft::TreeDecorator& dec) {
        const u16 shelf = static_cast<u16>(dec.a);
        if (random.next_float() >= dec.f[0]) return;
        const std::vector<Pos>& logs = work.logs;
        if (logs.empty()) return;
        if (logs.front().y == logs.back().y) {
            // A log that lies: on its two long sides.
            const bool along_x = logs.front().x != logs.back().x;
            const u32 sides[2] = {along_x ? world::kNorth : world::kEast, along_x ? world::kSouth : world::kWest};
            for (usize i = 0; i < logs.size(); ++i)
                for (const u32 facing : sides) {
                    if (random.next_float() > 0.25f) continue;
                    const Pos p{logs[i].x + world::kStepX[facing], logs[i].y, logs[i].z + world::kStepZ[facing]};
                    if (!shelf_may_replace(p)) continue;
                    if (has_shelf_beside(p, shelf) || has_shelf_beside(logs[i], shelf)) continue;
                    place_shelf(p, facing, shelf);
                }
            return;
        }
        const u32 first = random_horizontal(random);
        const u32 sides[2] = {first, clockwise(first)};
        const i32 base_y = logs[0].y;
        for (usize i = 0; i < logs.size(); ++i) {
            const i32 dy = logs[i].y - base_y;
            if (dy < 1 || dy > 4) continue;
            for (const u32 facing : sides) {
                if (random.next_float() > 0.25f) continue;
                const Pos p{logs[i].x + world::kStepX[facing], logs[i].y, logs[i].z + world::kStepZ[facing]};
                if (!shelf_may_replace(p)) continue;
                if (has_shelf({p.x, p.y - 1, p.z}, shelf)) continue;
                place_shelf(p, facing, shelf);
                break;
            }
        }
    }

    // PaleMossDecorator
    void moss_hanger(Pos p, u16 moss) {
        while (is_air({p.x, p.y - 1, p.z})) {
            if (static_cast<double>(random.next_float()) >= 0.5) {
                set_decoration(p, prop::with_flag(moss, Prop::kTip, false));
                --p.y;
                continue;
            }
            break;
        }
        set_decoration(p, prop::with_flag(moss, Prop::kTip, true));
    }
    void pale_moss(const ft::TreeDecorator& dec) {
        const u16 moss = static_cast<u16>(dec.b);
        std::vector<Pos>& shuffled = work.list;
        shuffled = work.logs;
        shuffle(shuffled);
        if (shuffled.empty()) return;
        Pos origin = shuffled[0];  // Collections.min by height: the first of the lowest
        for (const Pos& p : shuffled)
            if (p.y < origin.y) origin = p;
        if (random.next_float() < dec.f[2]) d.place_feature(static_cast<u32>(dec.a), random, {origin.x, origin.y + 1, origin.z});
        for (usize i = 0; i < work.logs.size(); ++i) {
            const Pos log = work.logs[i];
            if (random.next_float() < dec.f[1] && is_air({log.x, log.y - 1, log.z})) moss_hanger({log.x, log.y - 1, log.z}, moss);
        }
        for (usize i = 0; i < work.leaves.size(); ++i) {
            const Pos leaf = work.leaves[i];
            if (random.next_float() < dec.f[0] && is_air({leaf.x, leaf.y - 1, leaf.z}))
                moss_hanger({leaf.x, leaf.y - 1, leaf.z}, moss);
        }
    }

    void creaking_heart(const ft::TreeDecorator& dec) {
        if (work.logs.empty()) return;
        if (random.next_float() >= dec.f[0]) return;
        std::vector<Pos>& places = work.list;
        places = work.logs;
        shuffle(places);
        for (const Pos& p : places) {
            bool within = true;
            for (u32 direction = 0; direction < 6 && within; ++direction)
                within = in_state_set(ft::kSetLogs, block(moved(p, direction)));
            if (!within) continue;
            set_decoration(p, static_cast<u16>(dec.a));
            return;
        }
    }

    void decorate(u32 first, u32 count) {
        for (u32 i = first; i < first + count; ++i) {
            const ft::TreeDecorator& dec = ft::kTreeDecorators[i];
            switch (dec.type) {
                case ft::TreeDecoratorType::kBeehive: beehive(dec); break;
                case ft::TreeDecoratorType::kCocoa: cocoa(dec); break;
                case ft::TreeDecoratorType::kLeaveVine: leave_vine(dec); break;
                case ft::TreeDecoratorType::kTrunkVine: trunk_vine(dec); break;
                case ft::TreeDecoratorType::kAlterGround: alter_ground(dec); break;
                case ft::TreeDecoratorType::kAttachedToLeaves: attached_to_leaves(dec); break;
                case ft::TreeDecoratorType::kAttachedToLogs: attached_to_logs(dec); break;
                case ft::TreeDecoratorType::kPlaceOnGround: place_on_ground(dec); break;
                case ft::TreeDecoratorType::kShelfMushroom: shelf_mushroom(dec); break;
                case ft::TreeDecoratorType::kPaleMoss: pale_moss(dec); break;
                case ft::TreeDecoratorType::kCreakingHeart: creaking_heart(dec); break;
            }
        }
    }

    // ------------------------------------------------------------- updateLeaves, updateShapeAtEdge
    using Box = ShapeBox;
    struct Shape : ShapeBits {  // (by position)
        void fill(Pos p) { ShapeBits::fill(p.x, p.y, p.z); }
        bool full(Pos p) const { return ShapeBits::full(p.x, p.y, p.z); }
    };

    // TreeFeature.updateLeaves: how far every leaf is from a log, as far as that is under seven -
    // counted outwards from the logs, a leaf at a time off the nearest set that has any.
    void update_leaves(Shape& shape, const Box& box) {
        const auto fill_set = [&](const JavaHashSet& set) {
            set.each([&](i32 x, i32 y, i32 z) {
                if (box.inside(x, y, z)) shape.fill({x, y, z});
            });
        };
        fill_set(work.decorations);
        fill_set(work.roots);
        JavaHashSet* to_check = work.to_check;
        for (u32 i = 0; i < 7; ++i) to_check[i].clear();
        work.trunks.each([&](i32 x, i32 y, i32 z) { to_check[0].add(x, y, z); });
        i32 smallest = 0;
        for (;;) {
            while (smallest < 7 && to_check[smallest].empty()) ++smallest;
            if (smallest >= 7) break;
            Pos p;
            to_check[smallest].pop_first(p.x, p.y, p.z);
            if (!box.inside(p.x, p.y, p.z)) continue;
            if (smallest != 0) {
                const u16 state = block(p);
                DCHECK(prop::has(state, Prop::kDistance));
                region.set_block(p.x, p.y, p.z, prop::with(state, Prop::kDistance, static_cast<u32>(smallest - 1)), 19);
            }
            shape.fill(p);
            for (u32 direction = 0; direction < 6; ++direction) {
                const Pos n = moved(p, direction);
                if (!box.inside(n.x, n.y, n.z) || shape.full(n)) continue;
                const i32 distance = optional_leaf_distance(block(n));
                if (distance < 0) continue;
                const i32 next = std::min(distance, smallest + 1);
                if (next >= 7) continue;
                to_check[next].add(n.x, n.y, n.z);
                smallest = std::min(smallest, next);
            }
        }
    }

    // TreeFeature.place
    bool place(Pos origin) {
        work.roots.clear();
        work.trunks.clear();
        work.foliage.clear();
        work.decorations.clear();
        if (!do_place(origin)) return false;
        if (work.trunks.empty() && work.foliage.empty()) return false;
        if (tree->decorator_count) {
            listed(work.roots, work.root_list);
            listed(work.trunks, work.logs);
            listed(work.foliage, work.leaves);
            decorate(tree->decorators, tree->decorator_count);
        }
        // BoundingBox.encapsulatingPositions of everything that was set.
        Box box{0, 0, 0, 0, 0, 0};
        bool any = false;
        const auto grow = [&](const JavaHashSet& set) {
            set.each([&](i32 x, i32 y, i32 z) {
                if (!any) {
                    box = {x, y, z, x, y, z};
                    any = true;
                    return;
                }
                box.x0 = std::min(box.x0, x), box.x1 = std::max(box.x1, x);
                box.y0 = std::min(box.y0, y), box.y1 = std::max(box.y1, y);
                box.z0 = std::min(box.z0, z), box.z1 = std::max(box.z1, z);
            });
        };
        grow(work.roots);
        grow(work.trunks);
        grow(work.foliage);
        grow(work.decorations);
        if (!any) return false;
        const usize cells = static_cast<usize>(box.xs()) * static_cast<usize>(box.ys()) * static_cast<usize>(box.zs());
        work.shape.assign((cells + 63) / 64, 0);
        Shape shape{work.shape, box};
        update_leaves(shape, box);
        update_shape_at_edge(region, shape, 2);  // (TreeFeature passes 3: less the bit that tells neighbours)
        return true;
    }
};

bool Decorator::place_tree(const ft::Tree& tree, FeatureRandom& random, Pos origin) {
    TreeBuilder builder{*this, region_, work_, random, &tree};
    return builder.place(origin);
}

// FallenTreeFeature: a stump, and beside it the log that fell - where it has ground to lie on.
bool Decorator::place_fallen_tree(const ft::FallenTree& fallen, FeatureRandom& random, Pos origin) {
    TreeBuilder builder{*this, region_, work_, random, nullptr};
    builder.keep_decorations = false;
    FeatureWork& work = work_;
    Region& region = region_;
    const ft::Provider& trunk = ft::kProviders[fallen.trunk_provider];
    // placeLogBlock: the block as a player's would be set, and what is above it left to be looked at again.
    const auto place_log_block = [&](Pos p, i32 axis) {
        u16 state = state_of(trunk, random, p);
        if (axis != kNoAxis) state = prop::with(state, Prop::kAxis, static_cast<u32>(axis));
        region.set_block(p.x, p.y, p.z, state, 3);
        mark_above(p.x, p.y, p.z);
    };
    const auto over_solid_ground = [&](Pos p) {
        return in_state_set(ft::kSetSturdy[world::kUp], region.block(p.x, p.y - 1, p.z));
    };
    const auto decorate_logs = [&](u32 first, u32 count) {
        if (count == 0) return;
        work.root_list.clear();
        work.leaves.clear();
        builder.decorate(first, count);
    };

    place_log_block(origin, kNoAxis);
    work.logs.assign(1, origin);  // (Set.of(stump))
    decorate_logs(fallen.stump_decorators, fallen.stump_decorator_count);
    const u32 direction = random_horizontal(random);
    const i32 length = sample(ft::kInts[fallen.log_length], random) - 2;
    const i32 away = 2 + random.next_int(2);
    Pos start = moved(origin, direction, away);
    // setGroundHeightForFallenLogStartPos: from one above, down to where the log may lie.
    ++start.y;
    for (i32 i = 0; i < 6; ++i) {
        if (builder.valid_tree_pos(start) && over_solid_ground(start)) break;
        --start.y;
    }
    // canPlaceEntireFallenLog: room all along, and no more than two blocks in a row over nothing.
    i32 gap = 0;
    Pos p = start;
    for (i32 i = 0; i < length; ++i) {
        if (!builder.valid_tree_pos(p)) return true;
        if (!over_solid_ground(p)) {
            if (++gap > 2) return true;
        } else {
            gap = 0;
        }
        p = moved(p, direction);
    }
    work.fallen.clear();
    p = start;
    for (i32 i = 0; i < length; ++i) {
        place_log_block(p, static_cast<i32>(prop::axis_of(direction)));
        work.fallen.add(p.x, p.y, p.z);
        p = moved(p, direction);
    }
    TreeBuilder::listed(work.fallen, work.logs);
    decorate_logs(fallen.log_decorators, fallen.log_decorator_count);
    return true;
}

}  // namespace worldgen
