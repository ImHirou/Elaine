#include "phys/physics.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "world/block.hpp"

#include PROTO_GEN_PHYSICS_HPP

namespace phys {

namespace {

namespace gp = proto::gen::physics;
using world::Aabb;
using world::BlockState;

struct BlockXyz {
    i32 x, y, z;
};

ALWAYS_INLINE i32 floor_i(double v) {  // Mth.floor
    const i32 i = static_cast<i32>(v);
    return v < static_cast<double>(i) ? i - 1 : i;
}
ALWAYS_INLINE bool mth_equal(double a, double b) { return std::fabs(b - a) < 9.999999747378752e-6; }  // Mth.equal
ALWAYS_INLINE const gp::Material& material(BlockState s) { return gp::kMaterials[gp::kState[s] >> gp::kMaterialShift]; }
ALWAYS_INLINE BlockState block_at(const World& w, BlockXyz p) { return w.open ? world::kAir : w.blocks.block(p.x, p.y, p.z); }

// World::near and open for this tick: what do the sections around the entity hold? "Around" is
// its box, where its velocity can take it in a tick (gravity included) or a step lift it, and
// two blocks more on every side - further than any block a tick reads: the cells beside the box
// it sweeps, the block under its feet.
ALWAYS_INLINE void look_around(World& w, const Kind& k, const Motion& m) {
    const double half = static_cast<double>(k.width / 2.0f), height = static_cast<double>(k.height + k.max_up_step);
    const double rx = std::fabs(m.vel[0]) + half, ry = std::fabs(m.vel[1]) + 0.1, rz = std::fabs(m.vel[2]) + half;
    if (UNLIKELY(!(rx < 64.0 && ry < 64.0 && rz < 64.0))) {  // (or a velocity that is not a number)
        w.near = world::kAskSections;
        w.open = false;
        return;
    }
    w.near = w.blocks.holds(floor_i(m.pos[0] - rx) - 2, floor_i(m.pos[1] - ry) - 2, floor_i(m.pos[2] - rz) - 2,
                            floor_i(m.pos[0] + rx) + 2, floor_i(m.pos[1] + height + ry) + 2, floor_i(m.pos[2] + rz) + 2);
    w.open = !(w.near & world::Dimension::kBlocks);
}
ALWAYS_INLINE Aabb box_of(const Kind& k, const double pos[3]) {
    return world::entity_box(pos[0], pos[1], pos[2], k.width, k.height);
}

// Entity.getOnPos(offset): the block `offset` below the feet, in the column of the block the
// entity is known to stand on if there is one. A fence or wall it stands on is that block
// itself: they are taller than their cell.
BlockXyz on_pos(const World& w, const Motion& m, const Body& b, float offset) {
    if (b.has_support) {
        const BlockXyz support{b.support[0], b.support[1], b.support[2]};
        if (offset > 1.0e-5f) {
            const u8 flags = material(block_at(w, support)).flags;
            if ((static_cast<double>(offset) <= 0.5 && (flags & gp::kFence)) || (flags & gp::kWallOrGate)) return support;
            return {support.x, floor_i(m.pos[1] - static_cast<double>(offset)), support.z};
        }
        return support;
    }
    return {floor_i(m.pos[0]), floor_i(m.pos[1] - static_cast<double>(offset)), floor_i(m.pos[2])};
}

// Entity.checkSupportingBlock: the block it stands on - what a sliver under its feet touches,
// nearest first; looked for where it came from if it slid off an edge this very tick.
void check_supporting_block(World& w, const Kind& k, const Motion& m, Body& b, bool on_ground, const double* movement) {
    if (!on_ground) {
        b.on_ground_no_blocks = false;
        b.has_support = false;
        return;
    }
    const Aabb box = box_of(k, m.pos);
    Aabb area{{box.min[0], box.min[1] - 1.0e-6, box.min[2]}, {box.max[0], box.min[1], box.max[2]}};
    i32 found[3];
    bool present = !w.open && world::supporting_block(w.blocks, area, m.pos[0], m.pos[1], m.pos[2], found, w.near);
    if (present || b.on_ground_no_blocks) {
        b.has_support = present;
    } else if (movement) {
        area = world::moved(area, -movement[0], 0.0, -movement[2]);
        present = !w.open && world::supporting_block(w.blocks, area, m.pos[0], m.pos[1], m.pos[2], found, w.near);
        b.has_support = present;
    }
    if (present) b.support[0] = found[0], b.support[1] = found[1], b.support[2] = found[2];
    b.on_ground_no_blocks = !present;
}

// Entity.getBlockSpeedFactor: the block it is in, or else the one it stands on (soul sand, honey).
float block_speed_factor(const World& w, const Kind& k, const Motion& m, const Body& b) {
    const float here = material(block_at(w, {floor_i(m.pos[0]), floor_i(m.pos[1]), floor_i(m.pos[2])})).speed_factor;
    if (static_cast<double>(here) != 1.0) return here;
    return material(block_at(w, on_pos(w, m, b, k.below_offset))).speed_factor;
}

// Entity.restituteMovementAfterCollisions: what a collision does to the velocity - gone along
// the axis that hit, unless something bounces: only a block does (slime), from below, and only
// what lands harder than gravity alone would bring it down.
void restitute(World& w, const Kind& k, Motion& m, Body& b, bool x_hit, bool z_hit, const double mv[3]) {
    double restitution = 0.0;  // Entity.getEntityBounciness: the bounciness attribute, 0 for every type
    const double cur[3] = {m.vel[0], m.vel[1], m.vel[2]};
    double after[3] = {cur[0], cur[1], cur[2]};
    if (x_hit) after[0] = -cur[0] * restitution;
    if (z_hit) after[2] = -cur[2] * restitution;
    bool bounced = restitution > 0 && (x_hit || z_hit);
    if (b.vertical_collision) {
        const double gravity = b.no_gravity ? 0.0 : k.gravity;  // getEffectiveGravity
        if (b.vertical_collision_below) {
            // (The block is only looked up when it is asked: Entity.move reads it for everything
            // that moves - getOnPosLegacy - and nothing but this uses it.)
            if (-cur[1] <= gravity) {
                restitution = 0.0;
            } else if (const gp::Material& mat = material(block_at(w, on_pos(w, m, b, 0.2f)));
                       mat.flags & gp::kSuppressesBounce) {
                restitution = 0.0;
            } else {
                // getBlockBounciness: four fifths of the block's for what is not alive
                const float block = k.living ? mat.bounce : mat.bounce * 0.8f;
                restitution = std::max(restitution, static_cast<double>(block));
            }
        }
        double gravity_back, drag;
        if (restitution > 0) {
            const double portion = mv[1] / cur[1];
            gravity_back = portion * gravity;
            drag = 1.0 + portion * (static_cast<double>(0.98f) - 1.0);  // Mth.lerp(portion, 1, getAirDrag())
            bounced = true;
        } else {
            gravity_back = 0;
            drag = 1;
        }
        after[1] = (gravity_back - cur[1]) * drag * restitution;
    }
    if (bounced) b.sync_position = true;  // (Block.bounceOn, GameEvent.BOUNCE: nothing listens)
    m.vel[0] = after[0], m.vel[1] = after[1], m.vel[2] = after[2];
}

// ---------------------------------------------------------------------------- blocks an entity is in

// A small set of block positions.
struct Seen {
    std::vector<u64>& v;
    static u64 key(i32 x, i32 y, i32 z) {
        return (static_cast<u64>(static_cast<u32>(x) & 0x3FFFFFFu) << 38) |
               (static_cast<u64>(static_cast<u32>(z) & 0x3FFFFFFu) << 12) | (static_cast<u32>(y) & 0xFFFu);
    }
    bool add(i32 x, i32 y, i32 z) {  // true if it was not in
        const u64 k = key(x, y, z);
        if (std::find(v.begin(), v.end(), k) != v.end()) return false;
        v.push_back(k);
        return true;
    }
};

// BlockPos.betweenCornersInDirection / betweenClosed: every block of the box between two
// corners. (The order vanilla walks them in only matters to a visitor that stops half way,
// and the one here stops for a whole batch or not at all.)
template <class F>
ALWAYS_INLINE bool each_block(i32 ax, i32 ay, i32 az, i32 bx, i32 by, i32 bz, F&& f) {
    for (i32 y = std::min(ay, by); y <= std::max(ay, by); ++y)
        for (i32 z = std::min(az, bz); z <= std::max(az, bz); ++z)
            for (i32 x = std::min(ax, bx); x <= std::max(ax, bx); ++x)
                if (!f(x, y, z)) return false;
    return true;
}

// AABB.clip(minX .. maxZ, from, to): where the line first enters the box, if it does.
bool clip_box(const double lo[3], const double hi[3], const double from[3], const double to[3], double hit[3]) {
    const double d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    double scale = 1.0;
    bool any = false;
    for (u32 a = 0; a < 3; ++a) {  // getDirection: x, y, z; clipPoint
        const u32 u = (a + 1) % 3, v = (a + 2) % 3;
        double point;
        if (d[a] > 1.0e-7)
            point = lo[a];
        else if (d[a] < -1.0e-7)
            point = hi[a];
        else
            continue;
        const double s = (point - from[a]) / d[a];
        const double pu = from[u] + s * d[u], pv = from[v] + s * d[v];
        if (0 < s && s < scale && lo[u] - 1.0e-7 < pu && pu < hi[u] + 1.0e-7 && lo[v] - 1.0e-7 < pv && pv < hi[v] + 1.0e-7) {
            scale = s;
            any = true;
        }
    }
    if (!any) return false;
    hit[0] = from[0] + scale * d[0], hit[1] = from[1] + scale * d[1], hit[2] = from[2] + scale * d[2];
    return true;
}

// BlockGetter.forEachBlockIntersectedBetween: the blocks a box passes through on its way from
// `from` to `to` (`at_target`: the box where it ends) - those it starts in, those its leading
// corner crosses into on the way, those it ends in. visit(x, y, z, iteration) -> false stops.
template <class V>
bool blocks_between(const double from[3], const double to[3], const Aabb& at_target, std::vector<u64>& scratch, V&& visit) {
    const double travel[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    if (travel[0] * travel[0] + travel[1] * travel[1] + travel[2] * travel[2] <
        9.999999439624929e-11) {  // (double) Mth.square(1.0E-5f)
        return each_block(floor_i(at_target.min[0]), floor_i(at_target.min[1]), floor_i(at_target.min[2]),
                          floor_i(at_target.max[0]), floor_i(at_target.max[1]), floor_i(at_target.max[2]),
                          [&](i32 x, i32 y, i32 z) { return visit(x, y, z, 0); });
    }
    scratch.clear();
    Seen seen{scratch};
    const Aabb at_start = world::moved(at_target, travel[0] * -1.0, travel[1] * -1.0, travel[2] * -1.0);
    if (!each_block(floor_i(at_start.min[0]), floor_i(at_start.min[1]), floor_i(at_start.min[2]), floor_i(at_start.max[0]),
                    floor_i(at_start.max[1]), floor_i(at_start.max[2]), [&](i32 x, i32 y, i32 z) {
                        if (!visit(x, y, z, 0)) return false;
                        seen.add(x, y, z);
                        return true;
                    }))
        return false;

    // addCollisionsAlongTravel: the box's leading corner, block by block.
    const double size[3] = {at_target.max[0] - at_target.min[0], at_target.max[1] - at_target.min[1],
                            at_target.max[2] - at_target.min[2]};
    // getFurthestCorner
    const double x_dot = std::fabs(1.0 * travel[0] + 0.0 * travel[1] + 0.0 * travel[2]);
    const double y_dot = std::fabs(0.0 * travel[0] + 1.0 * travel[1] + 0.0 * travel[2]);
    const double z_dot = std::fabs(0.0 * travel[0] + 0.0 * travel[1] + 1.0 * travel[2]);
    const i32 xs = travel[0] < 0 ? -1 : 1, ys = travel[1] < 0 ? -1 : 1, zs = travel[2] < 0 ? -1 : 1;
    i32 corner[3];
    if (!(x_dot > y_dot) && !(x_dot > z_dot))
        corner[0] = -xs, corner[1] = -zs, corner[2] = ys;
    else if (!(y_dot > z_dot))
        corner[0] = zs, corner[1] = -ys, corner[2] = -xs;
    else
        corner[0] = -ys, corner[1] = xs, corner[2] = -zs;
    double to_corner[3], from_corner[3];
    for (u32 a = 0; a < 3; ++a) {
        const double centre =
            at_target.min[a] + 0.5 * (at_target.max[a] - at_target.min[a]);  // AABB.getCenter: Mth.lerp(0.5, min, max)
        to_corner[a] = centre + size[a] * 0.5 * static_cast<double>(corner[a]);
        from_corner[a] = to_corner[a] + -travel[a];  // Vec3.subtract
    }
    i32 cell[3] = {floor_i(from_corner[0]), floor_i(from_corner[1]), floor_i(from_corner[2])};
    i32 sign[3];
    double t_delta[3], t[3];
    for (u32 a = 0; a < 3; ++a) {
        sign[a] = travel[a] == 0 ? 0 : travel[a] > 0 ? 1 : -1;  // Mth.sign
        t_delta[a] = sign[a] == 0 ? 1.7976931348623157e308 : static_cast<double>(sign[a]) / travel[a];
        const double frac = from_corner[a] - std::floor(from_corner[a]);  // Mth.frac
        t[a] = t_delta[a] * (sign[a] > 0 ? 1.0 - frac : frac);
    }
    i32 iterations = 0;
    while (t[0] <= 1.0 || t[1] <= 1.0 || t[2] <= 1.0) {
        const u32 a = t[0] < t[1] ? (t[0] < t[2] ? 0u : 2u) : (t[1] < t[2] ? 1u : 2u);
        cell[a] += sign[a];
        t[a] += t_delta[a];
        const double lo[3] = {static_cast<double>(cell[0]), static_cast<double>(cell[1]), static_cast<double>(cell[2])};
        const double hi[3] = {static_cast<double>(cell[0] + 1), static_cast<double>(cell[1] + 1),
                              static_cast<double>(cell[2] + 1)};
        double hit[3];
        if (!clip_box(lo, hi, from_corner, to_corner, hit)) continue;
        ++iterations;
        i32 opposite[3];
        for (u32 k = 0; k < 3; ++k) {
            // Mth.clamp(hit, cell + 1e-5f, cell + 1 - 1e-5f)
            const double low = static_cast<double>(cell[k]) + 9.999999747378752e-6;
            const double high = static_cast<double>(cell[k]) + 1.0 - 9.999999747378752e-6;
            const double clamped = hit[k] < low ? low : std::min(hit[k], high);
            opposite[k] = floor_i(clamped - size[k] * static_cast<double>(corner[k]));
        }
        const i32 now = iterations;
        if (!each_block(cell[0], cell[1], cell[2], opposite[0], opposite[1], opposite[2],
                        [&](i32 x, i32 y, i32 z) { return !seen.add(x, y, z) || visit(x, y, z, now); }))
            return false;
    }
    return each_block(floor_i(at_target.min[0]), floor_i(at_target.min[1]), floor_i(at_target.min[2]), floor_i(at_target.max[0]),
                      floor_i(at_target.max[1]), floor_i(at_target.max[2]),
                      [&](i32 x, i32 y, i32 z) { return !seen.add(x, y, z) || visit(x, y, z, iterations + 1); });
}

// HoneyBlock.entityInside: what falls along the side of a honey block slides down it slowly.
void honey_inside(const Kind& k, Motion& m, Body& b, i32 bx, i32 by, i32 bz) {
    constexpr double kDrag = 0.9800000190734863;
    const auto old_delta_y = [](double y) { return y / kDrag + 0.08; };
    const auto new_delta_y = [](double y) { return (y - 0.08) * kDrag; };
    // isSlidingDown
    if (b.on_ground) return;
    if (m.pos[1] > static_cast<double>(by) + 0.9375 - 1.0e-7) return;
    if (!(old_delta_y(m.vel[1]) < -0.08)) return;
    const double dx = std::fabs(static_cast<double>(bx) + 0.5 - m.pos[0]),
                 dz = std::fabs(static_cast<double>(bz) + 0.5 - m.pos[2]);
    const double overlap = 0.4375 + static_cast<double>(k.width / 2.0f);
    if (!(dx + 1.0e-7 > overlap || dz + 1.0e-7 > overlap)) return;
    // doSlideMovement
    if (old_delta_y(m.vel[1]) < -0.13) {
        const double factor = -0.05 / old_delta_y(m.vel[1]);
        m.vel[0] = m.vel[0] * factor;
        m.vel[2] = m.vel[2] * factor;
    }
    m.vel[1] = new_delta_y(-0.05);
    b.fall_distance = 0;
}

// Entity.checkInsideBlocks(from, to, ...) for one straight leg: every block the box passes
// through that is not air is told (BlockState.entityInside), once a tick. Returns the
// iterations it took, plus one.
i32 check_inside_leg(World& w, const Kind& k, Motion& m, Body& b, const double from[3], const double to[3], i32 max_iterations,
                     Seen& told) {
    // (deflate(1.0E-5f); the box at the target is what decides `isPrecise`, which no block here asks for)
    const Aabb at_target = world::inflated(box_of(k, to), -9.999999747378752e-6, -9.999999747378752e-6, -9.999999747378752e-6);
    i32 last = 0;
    blocks_between(from, to, at_target, w.scratch.along, [&](i32 x, i32 y, i32 z, i32 iteration) {
        if (iteration >= max_iterations) return false;
        last = iteration;
        const BlockState s = w.blocks.block(x, y, z);  // (not block_at: a movement of last tick may lie outside this tick's air)
        if (LIKELY(s == world::kAir)) return true;
        const u8 inside = material(s).inside;
        if (LIKELY(inside == gp::kInsideNone))
            return true;  // (every block's getEntityInsideCollisionShape here is the full block)
        if (!told.add(x, y, z)) return true;
        if (inside == gp::kInsideWeb) {  // WebBlock.entityInside: Entity.makeStuckInBlock
            b.fall_distance = 0;
            b.stuck[0] = 0.25, b.stuck[1] = 0.05000000074505806, b.stuck[2] = 0.25;
        } else {
            honey_inside(k, m, b, x, y, z);
        }
        return true;
    });
    return last + 1;
}

// Entity.applyEffectsFromBlocks(List<Movement>)
void apply_effects(World& w, const Kind& k, Motion& m, Body& b, const Movement* movements, u32 count) {
    if (b.no_physics) return;                           // isAffectedByBlocks
    if (LIKELY(!w.blocks.has_entity_blocks())) return;  // no block in the dimension does anything to an entity in or on it
    {
        // ...or none where it has been this tick: the sections that its box touched on the way,
        // with a block to spare all round (the block it stands on is under its feet).
        double lo[3] = {m.pos[0], m.pos[1], m.pos[2]}, hi[3] = {m.pos[0], m.pos[1], m.pos[2]};
        for (u32 i = 0; i < count; ++i) {
            for (u32 a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], std::min(movements[i].from[a], movements[i].to[a]));
                hi[a] = std::max(hi[a], std::max(movements[i].from[a], movements[i].to[a]));
            }
        }
        const double half = static_cast<double>(k.width / 2.0f), height = static_cast<double>(k.height);
        if (!(w.blocks.holds(floor_i(lo[0] - half) - 1, floor_i(lo[1]) - 2, floor_i(lo[2] - half) - 1, floor_i(hi[0] + half) + 1,
                             floor_i(hi[1] + height) + 1, floor_i(hi[2] + half) + 1) &
              world::Dimension::kEntityBlocks))
            return;
    }
    if (b.on_ground) {  // Block.stepOn
        const BlockState s = block_at(w, on_pos(w, m, b, 0.2f));
        if (material(s).flags & gp::kStepSlime) {  // SlimeBlock.stepOn
            const double abs_y = std::fabs(m.vel[1]);
            if (abs_y < 0.1) {
                const double scale = 0.4 + abs_y * 0.2;
                m.vel[0] = m.vel[0] * scale, m.vel[1] = m.vel[1] * 1.0, m.vel[2] = m.vel[2] * scale;
            }
        }
    }
    // checkInsideBlocks(movements, collector)
    w.scratch.told.clear();
    Seen told{w.scratch.told};
    for (u32 i = 0; i < count; ++i) {
        const Movement& mv = movements[i];
        const double delta[3] = {mv.to[0] - mv.from[0], mv.to[1] - mv.from[1], mv.to[2] - mv.from[2]};
        i32 left = 16;
        if (mv.ordered && delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] > 0) {
            double pos[3] = {mv.from[0], mv.from[1], mv.from[2]};
            const u32 order[3] = {1, mv.z_first ? 2u : 0u, mv.z_first ? 0u : 2u};
            for (const u32 axis : order) {
                if (delta[axis] == 0) continue;
                double to[3];  // Vec3.relative(axis.getPositive(), axisMove)
                for (u32 a = 0; a < 3; ++a) to[a] = pos[a] + delta[axis] * (a == axis ? 1.0 : 0.0);
                left -= check_inside_leg(w, k, m, b, pos, to, left, told);
                pos[0] = to[0], pos[1] = to[1], pos[2] = to[2];
            }
        } else {
            left -= check_inside_leg(w, k, m, b, mv.from, mv.to, 16, told);
        }
        if (left <= 0) check_inside_leg(w, k, m, b, mv.to, mv.to, 1, told);
    }
}

// Entity.moveTowardsClosestSpace: an item inside a block is shot out through the nearest side
// that is not a full block (never downwards).
void move_towards_closest_space(World& w, Motion& m, double x, double y, double z, float (*random_float)(void*), void* rng) {
    const i32 px = floor_i(x), py = floor_i(y), pz = floor_i(z);
    const double delta[3] = {x - static_cast<double>(px), y - static_cast<double>(py), z - static_cast<double>(pz)};
    struct Side {
        i32 dx, dy, dz;
        u32 axis;
        bool positive;
    };
    static constexpr Side kSides[5] = {
        {0, 0, -1, 2, false}, {0, 0, 1, 2, true}, {-1, 0, 0, 0, false}, {1, 0, 0, 0, true}, {0, 1, 0, 1, true}};
    u32 axis = 1;
    bool positive = true;  // Direction.UP
    double closest = 1.7976931348623157e308;
    for (const Side& s : kSides) {
        const BlockState state = w.blocks.block(px + s.dx, py + s.dy, pz + s.dz);
        if ((gp::kState[state] & gp::kShapeMask) == gp::kShapeCube) continue;  // isCollisionShapeFullBlock
        const double d = delta[s.axis];
        const double oriented = s.positive ? 1.0 - d : d;
        if (oriented < closest) {
            closest = oriented;
            axis = s.axis;
            positive = s.positive;
        }
    }
    const float speed = random_float(rng) * 0.2f + 0.1f;
    const float step = positive ? 1.0f : -1.0f;
    for (u32 a = 0; a < 3; ++a) m.vel[a] = m.vel[a] * 0.75;
    m.vel[axis] = static_cast<double>(step * speed);
}

}  // namespace

void move(World& w, const Kind& k, Motion& m, Body& b, double dx, double dy, double dz) {
    if (b.no_physics) {
        m.pos[0] = m.pos[0] + dx, m.pos[1] = m.pos[1] + dy, m.pos[2] = m.pos[2] + dz;
        b.horizontal_collision = b.vertical_collision = b.vertical_collision_below = false;
        return;
    }
    if (b.stuck[0] * b.stuck[0] + b.stuck[1] * b.stuck[1] + b.stuck[2] * b.stuck[2] > 1.0e-7) {
        dx = dx * b.stuck[0], dy = dy * b.stuck[1], dz = dz * b.stuck[2];
        b.stuck[0] = b.stuck[1] = b.stuck[2] = 0;
        m.vel[0] = m.vel[1] = m.vel[2] = 0;
    }
    const double delta[3] = {dx, dy, dz};
    double mv[3] = {dx, dy, dz};
    if (!w.open) world::collide(w.blocks, box_of(k, m.pos), delta, k.max_up_step, b.on_ground, mv, w.scratch.collide, w.near);
    const double len2 = mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2];
    if (len2 > 1.0e-7 || (dx * dx + dy * dy + dz * dz) - len2 < 1.0e-7) {
        if (UNLIKELY(b.fall_distance != 0 && len2 >= 1)) {
            // A fast fall looks up to eight blocks ahead for what breaks it (a cobweb).
            const double length = std::sqrt(len2);
            const double check = std::min(length, 8.0);
            const double to[3] = {m.pos[0] + mv[0] / length * check, m.pos[1] + mv[1] / length * check,
                                  m.pos[2] + mv[2] / length * check};
            if (world::clip_hits_fall_resetting(w.blocks, m.pos, to)) b.fall_distance = 0;
        }
        Movement& rec = b.last[0];
        for (u32 a = 0; a < 3; ++a) {
            rec.from[a] = m.pos[a];
            m.pos[a] = m.pos[a] + mv[a];
            rec.to[a] = m.pos[a];
        }
        rec.ordered = true;
        rec.z_first = std::fabs(dx) < std::fabs(dz);
        b.moves = 1;
    }
    const bool x_hit = !mth_equal(dx, mv[0]), z_hit = !mth_equal(dz, mv[2]);
    b.horizontal_collision = x_hit || z_hit;
    const bool moved_vertically = std::fabs(dy) > 0;
    // (On the server every entity but a player's own body is "authoritative": always judged.)
    b.vertical_collision = dy != mv[1];
    b.vertical_collision_below = b.vertical_collision && dy < 0;
    b.on_ground = b.vertical_collision_below;
    check_supporting_block(w, k, m, b, b.on_ground, mv);
    // checkFallDamage
    if (mv[1] < 0) b.fall_distance = b.fall_distance - static_cast<double>(static_cast<float>(mv[1]));
    if (b.on_ground) b.fall_distance = 0;  // (Block.fallOn first: damage, which nothing takes)
    if ((moved_vertically && b.vertical_collision) || b.horizontal_collision) restitute(w, k, m, b, x_hit, z_hit, mv);
    const double factor = static_cast<double>(block_speed_factor(w, k, m, b));
    m.vel[0] = m.vel[0] * factor, m.vel[1] = m.vel[1] * 1.0, m.vel[2] = m.vel[2] * factor;
}

void apply_effects_from_blocks(World& w, const Kind& k, Motion& m, Body& b, const double old_pos[3]) {
    b.finals = b.moves;  // what move() recorded this tick is final now
    b.moves = 0;
    if (b.finals == 0) {
        Movement& f = b.last[0];
        for (u32 a = 0; a < 3; ++a) f.from[a] = old_pos[a], f.to[a] = m.pos[a];
        f.ordered = false;
        b.finals = 1;
    } else {
        const double* to = b.last[0].to;
        const double dx = to[0] - m.pos[0], dy = to[1] - m.pos[1], dz = to[2] - m.pos[2];
        if (dx * dx + dy * dy + dz * dz > 9.999999439624929e-11) {
            Movement& f = b.last[1];
            for (u32 a = 0; a < 3; ++a) f.from[a] = to[a], f.to[a] = m.pos[a];
            f.ordered = false;
            b.finals = 2;
        }
    }
    apply_effects(w, k, m, b, b.last, b.finals);
}

void apply_effects_from_last_movements(World& w, const Kind& k, Motion& m, Body& b) {
    apply_effects(w, k, m, b, b.last, b.finals);
}

Fate item_tick(World& w, Motion& m, Body& b, u32 tick_count, i32 id, float (*random_float)(void*), void* rng) {
    // Entity.baseTick: checkBelowWorld
    if (UNLIKELY(m.pos[1] < static_cast<double>(w.blocks.min_y() - 64))) return Fate::kGone;
    const double old_pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
    const double old_vel[3] = {m.vel[0], m.vel[1], m.vel[2]};
    if (!b.no_gravity)
        m.vel[0] = m.vel[0] + 0.0, m.vel[1] = m.vel[1] + -kItem.gravity, m.vel[2] = m.vel[2] + 0.0;  // applyGravity
    look_around(w, kItem, m);

    const Aabb box = box_of(kItem, m.pos);
    b.no_physics = !w.open && !world::no_collision(w.blocks, world::inflated(box, -1.0e-7, -1.0e-7, -1.0e-7), w.near);
    if (UNLIKELY(b.no_physics))
        move_towards_closest_space(w, m, m.pos[0], (box.min[1] + box.max[1]) / 2.0, m.pos[2], random_float, rng);

    if (!b.on_ground || m.vel[0] * m.vel[0] + m.vel[2] * m.vel[2] > 9.999999747378752e-6 ||
        (static_cast<i32>(tick_count) + id) % 4 == 0) {
        move(w, kItem, m, b, m.vel[0], m.vel[1], m.vel[2]);
        apply_effects_from_blocks(w, kItem, m, b, old_pos);
        const float air_drag = 0.98f;
        float ground_friction = air_drag;
        if (b.on_ground) ground_friction *= material(block_at(w, on_pos(w, m, b, kItem.below_offset))).friction;
        m.vel[0] = m.vel[0] * static_cast<double>(ground_friction);
        m.vel[1] = m.vel[1] * static_cast<double>(air_drag);
        m.vel[2] = m.vel[2] * static_cast<double>(ground_friction);
        if (b.on_ground && m.vel[1] < 0) m.vel[0] = m.vel[0] * 1.0, m.vel[1] = m.vel[1] * -0.5, m.vel[2] = m.vel[2] * 1.0;
    } else {
        apply_effects_from_last_movements(w, kItem, m, b);
    }
    // Its velocity changed by more than a tenth of a block a tick: viewers hear of it at once.
    const double dx = m.vel[0] + -old_vel[0], dy = m.vel[1] + -old_vel[1], dz = m.vel[2] + -old_vel[2];
    if (dx * dx + dy * dy + dz * dz > 0.01) b.needs_sync = true;
    return Fate::kStays;
}

void tnt_tick(World& w, Motion& m, Body& b) {
    const double old_pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
    if (!b.no_gravity) m.vel[0] = m.vel[0] + 0.0, m.vel[1] = m.vel[1] + -kTnt.gravity, m.vel[2] = m.vel[2] + 0.0;  // applyGravity
    look_around(w, kTnt, m);
    move(w, kTnt, m, b, m.vel[0], m.vel[1], m.vel[2]);
    apply_effects_from_blocks(w, kTnt, m, b, old_pos);
    const double drag = static_cast<double>(0.98f);  // getAirDrag
    m.vel[0] = m.vel[0] * drag, m.vel[1] = m.vel[1] * drag, m.vel[2] = m.vel[2] * drag;
    if (b.on_ground) m.vel[0] = m.vel[0] * 0.7, m.vel[1] = m.vel[1] * -0.5, m.vel[2] = m.vel[2] * 0.7;
}

void falling_move(World& w, Motion& m, Body& b) {
    const double old_pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
    if (!b.no_gravity) m.vel[0] = m.vel[0] + 0.0, m.vel[1] = m.vel[1] + -kFallingBlock.gravity, m.vel[2] = m.vel[2] + 0.0;
    look_around(w, kFallingBlock, m);
    move(w, kFallingBlock, m, b, m.vel[0], m.vel[1], m.vel[2]);
    apply_effects_from_blocks(w, kFallingBlock, m, b, old_pos);
}

Fate living_tick(World& w, const Kind& k, Motion& m, Body& b) {
    // Entity.baseTick: checkBelowWorld -> LivingEntity.onBelowWorld hurts it until it dies. Nothing has health: gone.
    if (UNLIKELY(m.pos[1] < static_cast<double>(w.blocks.min_y() - 64))) return Fate::kGone;
    const double old_pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
    // LivingEntity.aiStep: what is left of a velocity under 0.003 a tick is dropped.
    if (std::fabs(m.vel[0]) < 0.003) m.vel[0] = 0;
    if (std::fabs(m.vel[2]) < 0.003) m.vel[2] = 0;
    if (std::fabs(m.vel[1]) < 0.003) m.vel[1] = 0;
    look_around(w, k, m);

    // travel -> travelInAir, with no input.
    const auto modified = [](float friction, float modifier) {  // LivingEntity.computeModifiedFriction
        return std::clamp(1.0f - (1.0f - friction) * modifier, 0.0f, 1.0f);
    };
    const float block_friction =
        b.on_ground ? modified(material(block_at(w, on_pos(w, m, b, k.below_offset))).friction, 1.0f) : 1.0f;
    // handleRelativeFrictionAndCalculateMovement: moveRelative adds Vec3.ZERO for no input.
    m.vel[0] = m.vel[0] + 0.0, m.vel[1] = m.vel[1] + 0.0, m.vel[2] = m.vel[2] + 0.0;
    move(w, k, m, b, m.vel[0], m.vel[1], m.vel[2]);
    double y = m.vel[1];
    y -= b.no_gravity ? 0.0 : k.gravity;  // getEffectiveGravity
    const float air_drag = modified(0.91f, 1.0f);
    const float friction = block_friction * air_drag;
    const float vertical = modified(0.98f, 1.0f);
    m.vel[0] = m.vel[0] * static_cast<double>(friction);
    m.vel[1] = y * static_cast<double>(vertical);
    m.vel[2] = m.vel[2] * static_cast<double>(friction);
    apply_effects_from_blocks(w, k, m, b, old_pos);
    return Fate::kStays;
}

// ---------------------------------------------------------------------------- at rest

namespace {

ALWAYS_INLINE bool same_bits(double a, double b) { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }

bool same_movement(const Movement& a, const Movement& b) {
    for (u32 i = 0; i < 3; ++i)
        if (!same_bits(a.from[i], b.from[i]) || !same_bits(a.to[i], b.to[i])) return false;
    return a.ordered == b.ordered && (!a.ordered || a.z_first == b.z_first);
}

float drew(void* flag) {  // an item at rest draws no random number: only one stuck in a block does
    *static_cast<bool*>(flag) = true;
    return 0.5f;
}

}  // namespace

bool same_state(const Motion& a, const Body& x, const Motion& b, const Body& y) {
    for (u32 i = 0; i < 3; ++i) {
        if (!same_bits(a.pos[i], b.pos[i])) return false;
        if (!same_bits(a.vel[i], b.vel[i]) && !(a.vel[i] == 0 && b.vel[i] == 0)) return false;
        if (!same_bits(x.stuck[i], y.stuck[i])) return false;
    }
    if (!same_bits(x.fall_distance, y.fall_distance)) return false;
    if (x.has_support != y.has_support || x.on_ground_no_blocks != y.on_ground_no_blocks || x.on_ground != y.on_ground ||
        x.horizontal_collision != y.horizontal_collision || x.vertical_collision != y.vertical_collision ||
        x.vertical_collision_below != y.vertical_collision_below || x.no_physics != y.no_physics ||
        x.no_gravity != y.no_gravity || x.needs_sync != y.needs_sync || x.sync_position != y.sync_position ||
        x.moves != y.moves || x.finals != y.finals)
        return false;
    if (x.has_support && (x.support[0] != y.support[0] || x.support[1] != y.support[1] || x.support[2] != y.support[2]))
        return false;
    for (u32 i = 0; i < x.finals; ++i)
        if (!same_movement(x.last[i], y.last[i])) return false;
    return true;
}

bool item_rests(World& w, const Motion& m, const Body& b, u32 tick_count, i32 id, ItemRest& rest) {
    if (!b.on_ground || b.no_physics || b.no_gravity || b.sync_position) return false;
    if (!(std::fabs(m.vel[0]) < 1.0e-7 && std::fabs(m.vel[2]) < 1.0e-7)) return false;  // it still moves, if by a hair
    // What item_tick() multiplies by when the item is stopped by the ground it lies on.
    w.near = world::kAskSections, w.open = false;
    rest.factor = static_cast<double>(block_speed_factor(w, kItem, m, b));
    rest.friction = static_cast<double>(0.98f * material(block_at(w, on_pos(w, m, b, kItem.below_offset))).friction);
    Motion real = m, guess = m;
    Body ticked = b, kept = b;
    bool random = false;
    for (u32 t = 1; t <= 4; ++t) {
        ticked.needs_sync = kept.needs_sync = false;
        if (item_tick(w, real, ticked, tick_count + t, id, drew, &random) == Fate::kGone || random) return false;
        kept.needs_sync = item_rest_tick(guess.vel[0], guess.vel[1], guess.vel[2],
                                         static_cast<u32>(static_cast<i32>(tick_count + t) + id) & 3, rest);
        if (!same_state(real, ticked, guess, kept)) return false;
    }
    return true;
}

bool block_in_reach(const Motion& m, float width, float height, i32 x, i32 y, i32 z) {
    // BlockCollisions looks one cell beyond the box it is asked about, grown by 1e-7; a tick at
    // rest asks about the entity's box and about its fall of a tick, never more than 0.16 down.
    // Everything else such a tick reads - the block under it, the one it stands in - is inside.
    // One cell more for the neighbours of the block that was set.
    const Aabb box = world::entity_box(m.pos[0], m.pos[1], m.pos[2], width, height);
    const i32 at[3] = {x, y, z};
    for (u32 a = 0; a < 3; ++a) {
        const double low = box.min[a] - (a == 1 ? 0.25 : 1.0e-6), high = box.max[a] + 1.0e-6;
        if (at[a] < floor_i(low) - 2 || at[a] > floor_i(high) + 2) return false;
    }
    return true;
}

void set_on_ground(World& w, float width, float height, const Motion& m, Body& b, bool on_ground) {
    const Kind size{width, height, 0.0f, 0.0, 0.0f, false};  // only the box is asked for
    w.near = world::kAskSections, w.open = false;
    b.on_ground = on_ground;
    check_supporting_block(w, size, m, b, on_ground, nullptr);
}

bool push_impulse(const double a_pos[3], const double b_pos[3], double& xa, double& za) {
    xa = b_pos[0] - a_pos[0];
    za = b_pos[2] - a_pos[2];
    double dd = std::max(std::fabs(xa), std::fabs(za));  // Mth.absMax
    if (!(dd >= 0.009999999776482582)) return false;
    dd = std::sqrt(dd);
    xa = xa / dd;
    za = za / dd;
    double pow = 1.0 / dd;
    if (pow > 1) pow = 1;
    xa = xa * pow;
    za = za * pow;
    xa = xa * 0.05000000074505806;
    za = za * 0.05000000074505806;
    return true;
}

}  // namespace phys
