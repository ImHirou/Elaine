#include "world/collide.hpp"

#include <bit>
#include <cmath>

#include "world/block.hpp"

namespace world {

namespace {

constexpr double kEps = 1.0e-7;  // Shapes.EPSILON

i32 floor_i(double v) {
    const i32 i = static_cast<i32>(v);
    return v < static_cast<double>(i) ? i - 1 : i;
}

// The block shapes a box can meet (BlockCollisions): every cell the box reaches into, and one
// layer below - a shape may be taller than its block (a fence is 1.5).
template <class F>
void for_each_box(const Dimension& w, const Aabb& area, F&& f) {
    const i32 x0 = floor_i(area.min[0] - kEps), x1 = floor_i(area.max[0] + kEps);
    const i32 y0 = floor_i(area.min[1] - kEps) - 1, y1 = floor_i(area.max[1] + kEps);
    const i32 z0 = floor_i(area.min[2] - kEps), z1 = floor_i(area.max[2] + kEps);
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 x = x0; x <= x1; ++x) {
                const BlockState s = w.block(x, y, z);
                if (LIKELY(s == kAir)) continue;
                bool dynamic = false;
                for (const Box& b : collision_boxes(s, dynamic))
                    f(Aabb{{x + b.min[0], y + b.min[1], z + b.min[2]}, {x + b.max[0], y + b.max[1], z + b.max[2]}}, x, y, z);
            }
        }
    }
}

// VoxelShape.collideX for one box of a shape: how much of `distance` along `axis` the box
// leaves to `moving`.
double collide_axis(const Aabb& shape, const Aabb& moving, u32 axis, double distance) {
    const u32 b = (axis + 1) % 3, c = (axis + 2) % 3;
    if (!(moving.max[b] - kEps > shape.min[b] && moving.min[b] + kEps < shape.max[b] && moving.max[c] - kEps > shape.min[c] &&
          moving.min[c] + kEps < shape.max[c]))
        return distance;
    if (distance > 0 && shape.max[axis] > moving.max[axis] - kEps) {
        const double d = shape.min[axis] - moving.max[axis];
        if (d >= -kEps) distance = std::min(distance, d);
    } else if (distance < 0 && shape.min[axis] < moving.min[axis] + kEps) {
        const double d = shape.max[axis] - moving.min[axis];
        if (d <= kEps) distance = std::max(distance, d);
    }
    return distance;
}

}  // namespace

void collide(const Dimension& w, const Aabb& box, const double move[3], double out[3], std::vector<Aabb>& scratch) {
    out[0] = move[0], out[1] = move[1], out[2] = move[2];
    if (move[0] == 0 && move[1] == 0 && move[2] == 0) return;  // Entity.collide: lengthSqr() == 0
    Aabb swept = box;                                          // AABB.expandTowards(movement)
    for (u32 a = 0; a < 3; ++a) (move[a] < 0 ? swept.min[a] : swept.max[a]) += move[a];
    scratch.clear();
    for_each_box(w, swept, [&](const Aabb& b, i32, i32, i32) { scratch.push_back(b); });
    if (scratch.empty()) return;  // collideWithShapes: nothing in the way

    // Direction.axisStepOrder: y, then x before z unless z is the longer.
    const u32 order[3] = {1, std::fabs(move[0]) < std::fabs(move[2]) ? 2u : 0u,
                          std::fabs(move[0]) < std::fabs(move[2]) ? 0u : 2u};
    Aabb moving = box;
    out[0] = out[1] = out[2] = 0;
    for (const u32 axis : order) {
        double d = move[axis];
        if (d == 0) continue;
        for (const Aabb& b : scratch) {  // Shapes.collide
            if (std::fabs(d) < kEps) {
                d = 0;
                break;
            }
            d = collide_axis(b, moving, axis, d);
        }
        out[axis] = d;
        moving.min[axis] += d;
        moving.max[axis] += d;
    }
}

bool supporting_block(const Dimension& w, const Aabb& box, double x, double y, double z, i32 out[3]) {
    bool found = false;
    double best = 0;
    for_each_box(w, box, [&](const Aabb& b, i32 bx, i32 by, i32 bz) {
        // Shapes.joinIsNotEmpty(shape, box, AND): an overlap thinner than the margin is none.
        for (u32 a = 0; a < 3; ++a)
            if (std::min(box.max[a], b.max[a]) - std::max(box.min[a], b.min[a]) <= kEps) return;
        // BlockPos.distToCenterSqr(position); a tie goes to the smaller position (Vec3i.compareTo: y, z, x).
        const double dx = bx + 0.5 - x, dy = by + 0.5 - y, dz = bz + 0.5 - z;
        const double dist = dx * dx + dy * dy + dz * dz;
        if (!found || dist < best || (dist == best && (by != out[1] ? by < out[1] : bz != out[2] ? bz < out[2] : bx < out[0]))) {
            found = true;
            best = dist;
            out[0] = bx, out[1] = by, out[2] = bz;
        }
    });
    return found;
}

bool clip_hits(const Dimension& w, const double from[3], const double to[3]) {
    if (from[0] == to[0] && from[1] == to[1] && from[2] == to[2]) return false;
    const double d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    // VoxelShape.clip: a line that starts inside the shape hits it at once...
    const double probe[3] = {from[0] + d[0] * 0.001, from[1] + d[1] * 0.001, from[2] + d[2] * 0.001};
    const bool long_enough = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= kEps;
    const auto hits_block = [&](i32 x, i32 y, i32 z) {
        const BlockState s = w.block(x, y, z);
        if (LIKELY(s == kAir) || !long_enough) return false;
        bool dynamic = false;
        for (const Box& b : collision_boxes(s, dynamic)) {
            const double lo[3] = {x + b.min[0], y + b.min[1], z + b.min[2]}, hi[3] = {x + b.max[0], y + b.max[1], z + b.max[2]};
            if (probe[0] >= lo[0] && probe[0] < hi[0] && probe[1] >= lo[1] && probe[1] < hi[1] && probe[2] >= lo[2] &&
                probe[2] < hi[2])
                return true;
            // ...and otherwise where it enters through a face (AABB.clip: getDirection, clipPoint).
            for (u32 a = 0; a < 3; ++a) {
                if (!(std::fabs(d[a]) > kEps)) continue;
                const double t = ((d[a] > 0 ? lo[a] : hi[a]) - from[a]) / d[a];
                if (!(t > 0 && t < 1)) continue;
                const u32 u = (a + 1) % 3, v = (a + 2) % 3;
                const double pu = from[u] + t * d[u], pv = from[v] + t * d[v];
                if (pu > lo[u] - kEps && pu < hi[u] + kEps && pv > lo[v] - kEps && pv < hi[v] + kEps) return true;
            }
        }
        return false;
    };
    // BlockGetter.traverseBlocks: both ends pulled a hair towards each other, then cell by cell.
    double a[3], b[3];
    for (u32 k = 0; k < 3; ++k) {
        a[k] = from[k] + -kEps * (to[k] - from[k]);
        b[k] = to[k] + -kEps * (from[k] - to[k]);
    }
    i32 cell[3] = {floor_i(a[0]), floor_i(a[1]), floor_i(a[2])};
    if (hits_block(cell[0], cell[1], cell[2])) return true;
    i32 sign[3];
    double t_delta[3], t[3];
    for (u32 k = 0; k < 3; ++k) {
        const double dk = b[k] - a[k];
        sign[k] = dk > 0 ? 1 : dk < 0 ? -1 : 0;
        t_delta[k] = sign[k] == 0 ? 1.7976931348623157e308 : static_cast<double>(sign[k]) / dk;
        const double frac = a[k] - std::floor(a[k]);  // Mth.frac
        t[k] = t_delta[k] * (sign[k] > 0 ? 1.0 - frac : frac);
    }
    while (t[0] <= 1.0 || t[1] <= 1.0 || t[2] <= 1.0) {
        const u32 k = t[0] < t[1] ? (t[0] < t[2] ? 0u : 2u) : (t[1] < t[2] ? 1u : 2u);
        cell[k] += sign[k];
        t[k] += t_delta[k];
        if (hits_block(cell[0], cell[1], cell[2])) return true;
    }
    return false;
}

void blast_positions(const Dimension& w, double x, double y, double z, float radius, std::span<const float, kBlastRays> strengths,
                     BlastScratch& scratch, std::vector<BlockAt>& out) {
    constexpr float kStep = 0.3f, kLoss = 0.22500001f;
    constexpr double kStepD = 0.30000001192092896;  // (double) 0.3f
    constexpr double kHorizontalLimit = 3.0e7;      // Level.isInWorldBoundsHorizontal
    // What the rays take is a set (a HashSet<BlockPos> in vanilla): here a bit per block of the
    // cube a ray can reach - it loses 0.225 of at most 1.3 radii every 0.3 blocks. Next to it,
    // what each of those blocks resists with, looked up when a ray first comes by: the rays
    // pass some 25 000 points, three to a block and many rays through the same block near the
    // centre. Asking the world at every point instead costs 227 us for a TNT on flat ground,
    // this 175 (bench "blast", release, i5-9300H).
    const i32 reach = static_cast<i32>(radius * 1.3f / kLoss * kStep) + 2;
    const i32 side = 2 * reach + 1;
    const i32 cx = floor_i(x) - reach, cy = floor_i(y) - reach, cz = floor_i(z) - reach;
    constexpr float kUnknown = -2.0f, kNoResistance = -1.0f;  // air: Optional.empty()
    scratch.reached.assign(static_cast<usize>(side * side * side + 63) / 64, 0);
    scratch.resistance.assign(static_cast<usize>(side * side * side), kUnknown);
    u32 ray = 0;
    for (i32 xx = 0; xx < 16; ++xx) {
        for (i32 yy = 0; yy < 16; ++yy) {
            for (i32 zz = 0; zz < 16; ++zz) {
                if (xx != 0 && xx != 15 && yy != 0 && yy != 15 && zz != 0 && zz != 15) continue;
                double xd = static_cast<double>(static_cast<float>(xx) / 15.0f * 2.0f - 1.0f);
                double yd = static_cast<double>(static_cast<float>(yy) / 15.0f * 2.0f - 1.0f);
                double zd = static_cast<double>(static_cast<float>(zz) / 15.0f * 2.0f - 1.0f);
                const double d = std::sqrt(xd * xd + yd * yd + zd * zd);
                xd /= d, yd /= d, zd /= d;
                float power = radius * (0.7f + strengths[ray++] * 0.6f);
                double xp = x, yp = y, zp = z;
                for (; power > 0; power -= kLoss) {
                    const i32 bx = floor_i(xp), by = floor_i(yp), bz = floor_i(zp);
                    // Level.isInWorldBounds
                    if (by < w.min_y() || by > w.max_y() || std::fabs(static_cast<double>(bx)) >= kHorizontalLimit ||
                        std::fabs(static_cast<double>(bz)) >= kHorizontalLimit)
                        break;
                    const u32 cell = static_cast<u32>(((bx - cx) * side + (by - cy)) * side + (bz - cz));
                    DCHECK(cell < static_cast<u32>(side * side * side));
                    // ExplosionDamageCalculator.getBlockExplosionResistance: air does not resist.
                    // (Nor would a fluid: none can be in the world yet.)
                    float resistance = scratch.resistance[cell];
                    if (resistance == kUnknown) {
                        const BlockState state = w.block(bx, by, bz);
                        resistance = state == kAir || is_air(state) ? kNoResistance : block_info_of(state).explosion_resistance;
                        scratch.resistance[cell] = resistance;
                    }
                    if (resistance >= 0) power -= (resistance + kStep) * kStep;
                    if (power > 0) scratch.reached[cell >> 6] |= 1ull << (cell & 63);  // air too: it counts in the packet
                    xp += xd * kStepD;
                    yp += yd * kStepD;
                    zp += zd * kStepD;
                }
            }
        }
    }
    out.clear();
    for (u32 word = 0; word < scratch.reached.size(); ++word) {
        for (u64 bits = scratch.reached[word]; bits; bits &= bits - 1) {
            const i32 bit = static_cast<i32>(word * 64 + static_cast<u32>(std::countr_zero(bits)));
            out.push_back({cx + bit / (side * side), cy + bit / side % side, cz + bit % side});
        }
    }
}

}  // namespace world
