// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/collide.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include "world/block.hpp"

#include PROTO_GEN_PHYSICS_HPP

namespace world {

namespace {

namespace physics = proto::gen::physics;

constexpr double kEps = 1.0e-7;  // Shapes.EPSILON

ALWAYS_INLINE i32 floor_i(double v) {  // Mth.floor
    const i32 i = static_cast<i32>(v);
    return v < static_cast<double>(i) ? i - 1 : i;
}

// The block states of a small part of the world, read through the chunk that was asked last:
// a query's cells lie in one chunk, rarely in two or four.
class Cells {
  public:
    explicit Cells(const Dimension& w) : w_(w), min_y_(w.min_y()), sections_(w.sections()) {}
    ALWAYS_INLINE BlockState at(i32 x, i32 y, i32 z) {
        const i32 cx = x >> 4, cz = z >> 4;
        if (UNLIKELY(cx != cx_ || cz != cz_)) {
            cx_ = cx, cz_ = cz;
            chunk_ = w_.find(cx, cz);
        }
        const u32 si = static_cast<u32>((y - min_y_) >> 4);
        if (UNLIKELY(!chunk_ || si >= sections_)) return kAir;  // BlockCollisions skips what is not loaded
        return chunk_->sections[si].get(
            Section::index(static_cast<u32>(x & 15), static_cast<u32>(y & 15), static_cast<u32>(z & 15)));
    }

  private:
    const Dimension& w_;
    const Chunk* chunk_ = nullptr;
    i32 cx_ = INT32_MIN, cz_ = INT32_MIN;
    i32 min_y_;
    u32 sections_;
};

// ---------------------------------------------------------------------------- shapes
//
// A VoxelShape as vanilla's algorithms see one: per axis a list of coordinates, and the cells
// they cut, some of them full.
struct Voxels {
    const double* coords[3];          // n[a] + 1 of them
    double offset[3];                 // VoxelShape.move: an OffsetDoubleList adds it to every coordinate
    u32 n[3];                         // cells along each axis
    const unsigned long long* cells;  // bit (x * n[1] + y) * n[2] + z

    ALWAYS_INLINE double at(u32 axis, u32 i) const { return coords[axis][i] + offset[axis]; }
    // DiscreteVoxelShape.isFullWide: a cell outside the grid is empty.
    ALWAYS_INLINE bool full(i32 x, i32 y, i32 z) const {
        if (x < 0 || y < 0 || z < 0 || static_cast<u32>(x) >= n[0] || static_cast<u32>(y) >= n[1] || static_cast<u32>(z) >= n[2])
            return false;
        const u32 bit = (static_cast<u32>(x) * n[1] + static_cast<u32>(y)) * n[2] + static_cast<u32>(z);
        return (cells[bit >> 6] >> (bit & 63)) & 1;
    }
};

// A block's collision shape, moved to the block.
ALWAYS_INLINE Voxels voxels_of(u32 shape, i32 x, i32 y, i32 z) {
    const physics::Shape& s = physics::kShapes[shape];
    const double* c = physics::kShapeCoords + s.coords;
    return {{c, c + s.nx + 1, c + s.nx + s.ny + 2},
            {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)},
            {s.nx, s.ny, s.nz},
            physics::kShapeCells + s.cells};
}

// VoxelShape.findIndex: the cell `v` is in along an axis - -1 in front of the first, n behind the last.
ALWAYS_INLINE i32 find_index(const Voxels& s, u32 axis, double v) {
    const u32 n = s.n[axis];
    u32 i = 0;
    while (i <= n && !(v < s.at(axis, i))) ++i;  // Mth.binarySearch(0, n + 1, i -> v < get(i)): the lists ascend
    return static_cast<i32>(i) - 1;
}

// VoxelShape.min / max: where its full cells begin and end.
double shape_min(const Voxels& s, u32 axis) {
    const u32 b = (axis + 1) % 3, c = (axis + 2) % 3;
    i32 idx[3];
    for (u32 i = 0; i < s.n[axis]; ++i)
        for (u32 j = 0; j < s.n[b]; ++j)
            for (u32 k = 0; k < s.n[c]; ++k) {
                idx[axis] = static_cast<i32>(i), idx[b] = static_cast<i32>(j), idx[c] = static_cast<i32>(k);
                if (s.full(idx[0], idx[1], idx[2])) return s.at(axis, i);
            }
    return std::numeric_limits<double>::infinity();
}
double shape_max(const Voxels& s, u32 axis) {
    const u32 b = (axis + 1) % 3, c = (axis + 2) % 3;
    i32 idx[3];
    for (u32 i = s.n[axis]; i-- > 0;)
        for (u32 j = 0; j < s.n[b]; ++j)
            for (u32 k = 0; k < s.n[c]; ++k) {
                idx[axis] = static_cast<i32>(i), idx[b] = static_cast<i32>(j), idx[c] = static_cast<i32>(k);
                if (s.full(idx[0], idx[1], idx[2])) return s.at(axis, i + 1);
            }
    return -std::numeric_limits<double>::infinity();
}

// Shapes.create(box): the shape BlockCollisions makes of the box it asks about. A box whose
// corners all lie on eighths inside the unit cube becomes a grid of up to 8 cells an axis (and
// loses up to 1e-7 of its corners to the grid); any other box is one cell of its own size.
struct BoxShape {
    Voxels v;
    double coords[3][9];
    unsigned long long cells[8];
    bool made = false;
    bool empty = false;  // thinner than 1e-7 somewhere: Shapes.empty()
    bool plain = false;  // one cell, with the box's own coordinates (an ArrayVoxelShape)
};

i32 find_bits(double lo, double hi) {  // Shapes.findBits
    if (lo < -kEps || hi > 1.0000001) return -1;
    for (i32 bits = 0; bits <= 3; ++bits) {
        const i32 intervals = 1 << bits;
        const double a = lo * static_cast<double>(intervals), b = hi * static_cast<double>(intervals);
        // Math.round: the nearest whole number, halves upwards.
        if (std::fabs(a - std::floor(a + 0.5)) < kEps * static_cast<double>(intervals) &&
            std::fabs(b - std::floor(b + 0.5)) < kEps * static_cast<double>(intervals))
            return bits;
    }
    return -1;
}

NOINLINE void make_box_shape(const Aabb& box, BoxShape& s) {
    s.made = true;
    s.empty = box.max[0] - box.min[0] < kEps || box.max[1] - box.min[1] < kEps || box.max[2] - box.min[2] < kEps;
    if (s.empty) return;
    const i32 bits[3] = {find_bits(box.min[0], box.max[0]), find_bits(box.min[1], box.max[1]), find_bits(box.min[2], box.max[2])};
    s.v.cells = s.cells;
    for (u32 a = 0; a < 3; ++a) {
        s.v.coords[a] = s.coords[a];
        s.v.offset[a] = 0.0;
    }
    if (bits[0] < 0 || bits[1] < 0 || bits[2] < 0) {
        for (u32 a = 0; a < 3; ++a) {
            s.coords[a][0] = box.min[a];
            s.coords[a][1] = box.max[a];
            s.v.n[a] = 1;
        }
        s.cells[0] = 1;
        s.plain = true;
        return;
    }
    // A CubeVoxelShape (Shapes.block() itself when every axis has one cell): coordinate i of an
    // axis with `size` cells is i / size (CubePointRange), the cells between the rounded corners are full.
    i32 first[3], last[3];
    for (u32 a = 0; a < 3; ++a) {
        const i32 size = 1 << bits[a];
        s.v.n[a] = static_cast<u32>(size);
        for (i32 i = 0; i <= size; ++i) s.coords[a][i] = static_cast<double>(i) / static_cast<double>(size);
        first[a] = static_cast<i32>(std::floor(box.min[a] * static_cast<double>(size) + 0.5));
        last[a] = static_cast<i32>(std::floor(box.max[a] * static_cast<double>(size) + 0.5));
    }
    std::fill(std::begin(s.cells), std::end(s.cells), 0);
    for (i32 x = first[0]; x < last[0]; ++x)
        for (i32 y = first[1]; y < last[1]; ++y)
            for (i32 z = first[2]; z < last[2]; ++z) {
                const u32 bit = (static_cast<u32>(x) * s.v.n[1] + static_cast<u32>(y)) * s.v.n[2] + static_cast<u32>(z);
                s.cells[bit >> 6] |= 1ull << (bit & 63);
            }
}

// The cells two coordinate lists cut one axis into, each with the cell of either shape it lies in.
struct Merged {
    i32 first[64], second[64];
    u32 n = 0;
};

// Shapes.createIndexMerger(cost, first.getCoords(axis), second.getCoords(axis), false, false),
// run through forMergedIndexes. False when the two do not overlap along this axis at all
// (NonOverlappingMerger: no cell lies in both). `a` is a moved shape: its lists are
// OffsetDoubleLists, so the two are never both CubePointRanges and never a DiscreteCubeMerger.
bool merge_axis(const Voxels& a, const Voxels& b, u32 axis, Merged& out) {
    const u32 na = a.n[axis], nb = b.n[axis];
    out.n = 0;
    if (a.at(axis, na) < b.at(axis, 0) - kEps) return false;
    if (b.at(axis, nb) < a.at(axis, 0) - kEps) return false;
    if (na == nb) {  // IdenticalMerger: the same list
        bool same = true;
        for (u32 i = 0; i <= na && same; ++i) same = a.at(axis, i) == b.at(axis, i);
        if (same) {
            for (u32 i = 0; i < na; ++i) out.first[i] = out.second[i] = static_cast<i32>(i);
            out.n = na;
            return true;
        }
    }
    // IndirectMerger: the two lists merged; coordinates within 1e-7 of each other are one, and
    // (for AND) what comes before the other list has begun, or after it has ended, is left out.
    const u32 size_a = na + 1, size_b = nb + 1;
    DCHECK(size_a + size_b <= 64);
    double last = std::numeric_limits<double>::quiet_NaN();
    u32 ia = 0, ib = 0, r = 0;
    for (;;) {
        const bool out_a = ia >= size_a, out_b = ib >= size_b;
        if (out_a && out_b) break;
        const bool chose_a = !out_a && (out_b || a.at(axis, ia) < b.at(axis, ib) + kEps);
        if (chose_a) {
            ++ia;
            if (ib == 0 || out_b) continue;
        } else {
            ++ib;
            if (ia == 0 || out_a) continue;
        }
        const i32 ca = static_cast<i32>(ia) - 1, cb = static_cast<i32>(ib) - 1;
        const double next = chose_a ? a.at(axis, static_cast<u32>(ca)) : b.at(axis, static_cast<u32>(cb));
        if (!(last >= next - kEps)) {
            out.first[r] = ca;
            out.second[r] = cb;
            ++r;
            last = next;
        } else {
            out.first[r - 1] = ca;
            out.second[r - 1] = cb;
        }
    }
    out.n = r > 1 ? r - 1 : 0;  // forMergedIndexes: the cells between the coordinates
    return true;
}

// Shapes.joinIsNotEmpty(a, b, BooleanOp.AND) for two shapes that are not empty: is some cell in both?
NOINLINE bool join_is_not_empty(const Voxels& a, const Voxels& b) {
    for (u32 axis = 0; axis < 3; ++axis) {
        if (shape_max(a, axis) < shape_min(b, axis) - kEps) return false;
        if (shape_max(b, axis) < shape_min(a, axis) - kEps) return false;
    }
    Merged m[3];
    for (u32 axis = 0; axis < 3; ++axis)
        if (!merge_axis(a, b, axis, m[axis])) return false;
    for (u32 i = 0; i < m[0].n; ++i)
        for (u32 j = 0; j < m[1].n; ++j)
            for (u32 k = 0; k < m[2].n; ++k)
                if (a.full(m[0].first[i], m[1].first[j], m[2].first[k]) && b.full(m[0].second[i], m[1].second[j], m[2].second[k]))
                    return true;
    return false;
}

// The same along one axis for two shapes of one cell each, [a0, a1] and [e0, e1]: do the
// merged lists leave a cell that lies in both? The comparisons are merge_axis's, in its order,
// with what it does for two coordinates a list written out.
ALWAYS_INLINE bool cells_meet(double a0, double a1, double e0, double e1) {
    if (a1 < e0 - kEps) return false;
    if (e1 < a0 - kEps) return false;
    if (a0 == e0 && a1 == e1) return true;
    if (a0 < e0 + kEps) {                  // the block's first coordinate comes first, and is left out
        if (a1 < e0 + kEps) return false;  // ...its second too: it ends where the box begins
        // e0 is the first coordinate kept. Then the nearer of the two ends closes the cell -
        // unless it is within 1e-7 of e0, which makes it the same coordinate.
        return !(e0 >= (a1 < e1 + kEps ? a1 : e1) - kEps);
    }
    if (!(a0 < e1 + kEps)) return false;  // the box ends where the block begins
    return !(a0 >= (a1 < e1 + kEps ? a1 : e1) - kEps);
}

// ---------------------------------------------------------------------------- BlockCollisions
//
// f(collider) for every collision shape `box` meets, in the order vanilla's cursor walks the
// cells: x fastest, then y, then z, over the box grown by 1e-7 and by one more cell on every
// side. Cells on a face of that range only count for shapes that reach out of their block,
// cells on an edge only for a moving piston (whose shape depends on who asks: none here),
// corners not at all. Returns true as soon as f does.
//
// kLiteral: every step as vanilla takes it. Otherwise nothing is read at all when the
// sections around hold only air, the cells on the faces are read only when those sections may
// hold a shape that reaches out (a fence anywhere near makes a query read some 56 cells where
// 8 would do: the "fences" rows of bench "physics" are three to four times the others), and a
// shape of one cell is compared with the box by cells_meet(). (Bench "physics: items falling",
// i5-9300H: 194 ns a tick with every cell read, 138 with the sections asked first.)
template <bool kLiteral, class F>
ALWAYS_INLINE bool block_collisions(const Dimension& w, const Aabb& box, u32 near, F&& f) {
    const i32 x0 = floor_i(box.min[0] - kEps) - 1, x1 = floor_i(box.max[0] + kEps) + 1;
    const i32 y0 = floor_i(box.min[1] - kEps) - 1, y1 = floor_i(box.max[1] + kEps) + 1;
    const i32 z0 = floor_i(box.min[2] - kEps) - 1, z1 = floor_i(box.max[2] + kEps) + 1;
    i32 skip = 0;
    if (!kLiteral) {
        const u32 holds = near == kAskSections ? w.holds(x0, y0, z0, x1, y1, z1) : near;
        if (!(holds & Dimension::kBlocks)) return false;
        skip = (holds & Dimension::kLarge) ? 0 : 1;
    }
    Cells cells(w);
    BoxShape entity;
    for (i32 z = z0 + skip; z <= z1 - skip; ++z) {
        const u32 tz = (z == z0) | (z == z1);
        for (i32 y = y0 + skip; y <= y1 - skip; ++y) {
            const u32 tyz = tz + ((y == y0) | (y == y1));
            for (i32 x = x0 + skip; x <= x1 - skip; ++x) {
                const u32 type = tyz + ((x == x0) | (x == x1));
                if (type == 3) continue;
                const BlockState s = cells.at(x, y, z);
                if (LIKELY(s == kAir)) continue;
                const u32 p = physics::kState[s];
                const u32 kind = p & physics::kShapeMask;
                if (kind == physics::kShapeNone) continue;
                if (type == 1 && !(p & physics::kLarge)) continue;
                if (type == 2) continue;
                Collider c;
                c.x = x, c.y = y, c.z = z;
                const double bx = static_cast<double>(x), by = static_cast<double>(y), bz = static_cast<double>(z);
                if (kind == physics::kShapeCube) {
                    // if (blockShape == Shapes.block() && !box.intersects(x, y, z, x + 1, y + 1, z + 1)) continue;
                    c.box = {{bx, by, bz}, {bx + 1.0, by + 1.0, bz + 1.0}};
                    if (!intersects(box, c.box)) continue;
                    c.shape = kLiteral ? static_cast<u16>(state::collision_of(s)) : u16{0};
                } else {
                    // Shapes.joinIsNotEmpty(blockShape.move(pos), entityShape, BooleanOp.AND)
                    if (!entity.made) make_box_shape(box, entity);
                    if (entity.empty) continue;
                    const u32 shape = state::collision_of(s);
                    if (!kLiteral && kind == physics::kShapeBox && entity.plain) {
                        const double* b = physics::kShapeCoords + physics::kShapes[shape].coords;  // x0 x1 y0 y1 z0 z1
                        c.box = {{b[0] + bx, b[2] + by, b[4] + bz}, {b[1] + bx, b[3] + by, b[5] + bz}};
                        if (!(cells_meet(c.box.min[0], c.box.max[0], box.min[0], box.max[0]) &&
                              cells_meet(c.box.min[1], c.box.max[1], box.min[1], box.max[1]) &&
                              cells_meet(c.box.min[2], c.box.max[2], box.min[2], box.max[2])))
                            continue;
                        c.shape = 0;
                    } else {
                        if (!join_is_not_empty(voxels_of(shape, x, y, z), entity.v)) continue;
                        if (!kLiteral && kind == physics::kShapeBox) {
                            const double* b = physics::kShapeCoords + physics::kShapes[shape].coords;
                            c.box = {{b[0] + bx, b[2] + by, b[4] + bz}, {b[1] + bx, b[3] + by, b[5] + bz}};
                            c.shape = 0;
                        } else {
                            c.box = {{bx, by, bz}, {bx + 1.0, by + 1.0, bz + 1.0}};
                            c.shape = static_cast<u16>(shape);
                        }
                    }
                }
                if (f(c)) return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------- Shapes.collide

// VoxelShape.collideX for a shape of one cell: how much of `d` along `a` the cell leaves to
// `m`. What vanilla finds by findIndex over two coordinates is written out: the cell is in the
// way only if `m` overlaps it across (by more than the margin) and lies wholly in front of it.
ALWAYS_INLINE double collide_cell(const Aabb& s, const Aabb& m, u32 a, double d) {
    const u32 b = (a + 1) % 3, c = (a + 2) % 3;
    if (!(m.min[b] + kEps < s.max[b] && m.max[b] - kEps >= s.min[b] && m.min[c] + kEps < s.max[c] && m.max[c] - kEps >= s.min[c]))
        return d;
    if (d > 0) {
        if (m.max[a] - kEps < s.min[a]) {
            const double nd = s.min[a] - m.max[a];
            if (!(nd < -kEps)) d = nd < d ? nd : d;  // Math.min
        }
    } else if (d < 0) {
        if (m.min[a] + kEps >= s.max[a]) {
            const double nd = s.max[a] - m.min[a];
            if (!(nd > kEps)) d = nd > d ? nd : d;  // Math.max
        }
    }
    return d;
}

// VoxelShape.collideX: the first full cell in front of `m`, among the cells it overlaps across.
NOINLINE double collide_grid(const Voxels& s, const Aabb& m, u32 a, double d) {
    const u32 b = (a + 1) % 3, c = (a + 2) % 3;
    const double max_a = m.max[a], min_a = m.min[a];
    const i32 a_min = find_index(s, a, min_a + kEps), a_max = find_index(s, a, max_a - kEps);
    const i32 b_min = std::max(0, find_index(s, b, m.min[b] + kEps));
    const i32 b_max = std::min(static_cast<i32>(s.n[b]), find_index(s, b, m.max[b] - kEps) + 1);
    const i32 c_min = std::max(0, find_index(s, c, m.min[c] + kEps));
    const i32 c_max = std::min(static_cast<i32>(s.n[c]), find_index(s, c, m.max[c] - kEps) + 1);
    const i32 a_size = static_cast<i32>(s.n[a]);
    i32 idx[3];
    if (d > 0) {
        for (i32 i = a_max + 1; i < a_size; ++i)
            for (i32 j = b_min; j < b_max; ++j)
                for (i32 k = c_min; k < c_max; ++k) {
                    idx[a] = i, idx[b] = j, idx[c] = k;
                    if (!s.full(idx[0], idx[1], idx[2])) continue;
                    const double nd = s.at(a, static_cast<u32>(i)) - max_a;
                    if (!(nd < -kEps)) d = nd < d ? nd : d;
                    return d;
                }
    } else if (d < 0) {
        for (i32 i = a_min - 1; i >= 0; --i)
            for (i32 j = b_min; j < b_max; ++j)
                for (i32 k = c_min; k < c_max; ++k) {
                    idx[a] = i, idx[b] = j, idx[c] = k;
                    if (!s.full(idx[0], idx[1], idx[2])) continue;
                    const double nd = s.at(a, static_cast<u32>(i + 1)) - min_a;
                    if (!(nd > kEps)) d = nd > d ? nd : d;
                    return d;
                }
    }
    return d;
}

// Shapes.collide(axis, moving, shapes, distance)
ALWAYS_INLINE double collide_axis(std::span<const Collider> colliders, const Aabb& moving, u32 axis, double d) {
    for (const Collider& c : colliders) {
        if (std::fabs(d) < kEps) return 0.0;
        d = LIKELY(c.shape == 0) ? collide_cell(c.box, moving, axis, d)
                                 : collide_grid(voxels_of(c.shape, c.x, c.y, c.z), moving, axis, d);
    }
    return d;
}

// Entity.collideWithShapes
ALWAYS_INLINE void collide_with_shapes(const double move[3], const Aabb& box, std::span<const Collider> colliders,
                                       double out[3]) {
    if (colliders.empty()) {
        out[0] = move[0], out[1] = move[1], out[2] = move[2];
        return;
    }
    out[0] = out[1] = out[2] = 0.0;  // Vec3.ZERO
    // Direction.axisStepOrder: y, then x before z unless z is the longer.
    const bool z_first = std::fabs(move[0]) < std::fabs(move[2]);
    const u32 order[3] = {1, z_first ? 2u : 0u, z_first ? 0u : 2u};
    for (const u32 axis : order) {
        if (move[axis] == 0) continue;
        out[axis] = collide_axis(colliders, moved(box, out[0], out[1], out[2]), axis, move[axis]);
    }
}

// The y coordinates of a collider (VoxelShape.getCoords(Axis.Y)), in ascending order.
template <class F>
ALWAYS_INLINE void for_each_y(const Collider& c, F&& f) {
    if (LIKELY(c.shape == 0)) {
        if (f(c.box.min[1])) return;
        f(c.box.max[1]);
        return;
    }
    const Voxels v = voxels_of(c.shape, c.x, c.y, c.z);
    for (u32 i = 0; i <= v.n[1]; ++i)
        if (f(v.at(1, i))) return;
}

// Entity.collectCandidateStepUpHeights: every height a collider's grid has between the feet and
// `max_step` above them, but the one the plain move already ended at. A set, sorted.
u32 step_heights(const Aabb& grounded, std::span<const Collider> colliders, float max_step, float skip, float out[], u32 cap) {
    u32 n = 0;
    for (const Collider& c : colliders) {
        for_each_y(c, [&](double coord) {
            const float relative = static_cast<float>(coord - grounded.min[1]);
            if (relative < 0.0f || relative == skip) return false;
            if (relative > max_step) return true;
            for (u32 i = 0; i < n; ++i)  // FloatArraySet: by Float.floatToIntBits
                if (std::bit_cast<u32>(out[i]) == std::bit_cast<u32>(relative)) return false;
            if (n < cap) out[n++] = relative;
            return false;
        });
    }
    std::sort(out, out + n, [](float a, float b) {  // FloatArrays.unstableSort: Float.compare, -0 before 0
        return a < b || (a == b && std::signbit(a) && !std::signbit(b));
    });
    return n;
}

inline constexpr u32 kMaxStepHeights = 64;

template <bool kLiteral>
ALWAYS_INLINE void collide_impl(const Dimension& w, const Aabb& box, const double move[3], float max_up_step, bool on_ground,
                                double out[3], std::vector<Collider>& colliders, std::vector<Collider>& step_colliders,
                                u32 near) {
    out[0] = move[0], out[1] = move[1], out[2] = move[2];
    if (move[0] * move[0] + move[1] * move[1] + move[2] * move[2] == 0) return;  // movement.lengthSqr() == 0
    // collideBoundingBox
    colliders.clear();
    block_collisions<kLiteral>(w, expand_towards(box, move[0], move[1], move[2]), near, [&](const Collider& c) {
        colliders.push_back(c);
        return false;
    });
    collide_with_shapes(move, box, colliders, out);

    const bool x_hit = move[0] != out[0], y_hit = move[1] != out[1], z_hit = move[2] != out[2];
    const bool on_ground_after = y_hit && move[1] < 0;
    if (!(max_up_step > 0.0f && (on_ground_after || on_ground) && (x_hit || z_hit))) return;

    // Stepping up: from where the plain move left it on the ground, try every height the
    // colliders around offer, lowest first; the first that gets further sideways is taken.
    const Aabb grounded = on_ground_after ? moved(box, 0.0, out[1], 0.0) : box;
    Aabb area = expand_towards(grounded, move[0], static_cast<double>(max_up_step), move[2]);
    if (!on_ground_after) area = expand_towards(area, 0.0, -9.999999747378752e-6, 0.0);
    step_colliders.clear();
    block_collisions<kLiteral>(w, area, near, [&](const Collider& c) {
        step_colliders.push_back(c);
        return false;
    });
    float heights[kMaxStepHeights];
    const u32 n = step_heights(grounded, step_colliders, max_up_step, static_cast<float>(out[1]), heights, kMaxStepHeights);
    const double flat2 = out[0] * out[0] + out[2] * out[2];
    for (u32 i = 0; i < n; ++i) {
        const double attempt[3] = {move[0], static_cast<double>(heights[i]), move[2]};
        double stepped[3];
        collide_with_shapes(attempt, grounded, step_colliders, stepped);
        if (stepped[0] * stepped[0] + stepped[2] * stepped[2] > flat2) {
            const double to_ground = box.min[1] - grounded.min[1];
            out[0] = stepped[0] + -0.0;  // Vec3.subtract(0, distanceToGround, 0) is add(-0.0, -d, -0.0)
            out[1] = stepped[1] + -to_ground;
            out[2] = stepped[2] + -0.0;
            return;
        }
    }
}

template <bool kLiteral>
ALWAYS_INLINE bool supporting_impl(const Dimension& w, const Aabb& box, double x, double y, double z, i32 out[3], u32 near) {
    bool found = false;
    double best = std::numeric_limits<double>::max();
    block_collisions<kLiteral>(w, box, near, [&](const Collider& c) {
        // BlockPos.distToCenterSqr(position)
        const double dx = static_cast<double>(c.x) + 0.5 - x, dy = static_cast<double>(c.y) + 0.5 - y,
                     dz = static_cast<double>(c.z) + 0.5 - z;
        const double dist = dx * dx + dy * dy + dz * dz;
        // As near as the nearest so far: the one Vec3i.compareTo puts last stays (y, then z, then x).
        const bool later = !found || (out[1] != c.y ? out[1] < c.y : out[2] != c.z ? out[2] < c.z : out[0] < c.x);
        if (dist < best || (dist == best && later)) {
            found = true;
            best = dist;
            out[0] = c.x, out[1] = c.y, out[2] = c.z;
        }
        return false;
    });
    return found;
}

template <bool kLiteral>
ALWAYS_INLINE double spawn_offset_impl(const Dimension& w, i32 x, i32 y, i32 z, bool moved_up, const Aabb& box,
                                       std::vector<Collider>& colliders) {
    // new AABB(spawnPos), and the block under it as well when moved up
    const double bx = static_cast<double>(x), by = static_cast<double>(y), bz = static_cast<double>(z);
    Aabb cells{{bx, by, bz}, {bx + 1.0, by + 1.0, bz + 1.0}};
    if (moved_up) cells = expand_towards(cells, 0.0, -1.0, 0.0);
    colliders.clear();
    block_collisions<kLiteral>(w, cells, kAskSections, [&](const Collider& c) {
        colliders.push_back(c);
        return false;
    });
    return 1.0 + collide_axis(colliders, box, 1, moved_up ? -2.0 : -1.0);
}

}  // namespace

void collide(const Dimension& w, const Aabb& box, const double move[3], float max_up_step, bool on_ground, double out[3],
             CollideScratch& scratch, u32 near) {
    collide_impl<false>(w, box, move, max_up_step, on_ground, out, scratch.colliders, scratch.step_colliders, near);
}

bool no_collision(const Dimension& w, const Aabb& box, u32 near) {
    return !block_collisions<false>(w, box, near, [](const Collider&) { return true; });
}

bool supporting_block(const Dimension& w, const Aabb& box, double x, double y, double z, i32 out[3], u32 near) {
    return supporting_impl<false>(w, box, x, y, z, out, near);
}

double spawn_offset(const Dimension& w, i32 x, i32 y, i32 z, bool moved_up, const Aabb& box, CollideScratch& scratch) {
    return spawn_offset_impl<false>(w, x, y, z, moved_up, box, scratch.colliders);
}

namespace slow {

void collide(const Dimension& w, const Aabb& box, const double move[3], float max_up_step, bool on_ground, double out[3]) {
    std::vector<Collider> colliders, step_colliders;
    collide_impl<true>(w, box, move, max_up_step, on_ground, out, colliders, step_colliders, kAskSections);
}

bool no_collision(const Dimension& w, const Aabb& box) {
    return !block_collisions<true>(w, box, kAskSections, [](const Collider&) { return true; });
}

bool supporting_block(const Dimension& w, const Aabb& box, double x, double y, double z, i32 out[3]) {
    return supporting_impl<true>(w, box, x, y, z, out, kAskSections);
}

double spawn_offset(const Dimension& w, i32 x, i32 y, i32 z, bool moved_up, const Aabb& box) {
    std::vector<Collider> colliders;
    return spawn_offset_impl<true>(w, x, y, z, moved_up, box, colliders);
}

}  // namespace slow

// ---------------------------------------------------------------------------- lines

namespace {

// BlockGetter.clip: does the line from `from` to `to` hit a block? `boxes_of(state, out)` gives
// the shape a block has for this kind of line, as boxes in block coordinates.
template <class Shape>
ALWAYS_INLINE bool clip_impl(const Dimension& w, const double from[3], const double to[3], Shape&& boxes_of) {
    if (from[0] == to[0] && from[1] == to[1] && from[2] == to[2]) return false;
    const double d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    // VoxelShape.clip: a line that starts inside the shape hits it at once...
    const double probe[3] = {from[0] + d[0] * 0.001, from[1] + d[1] * 0.001, from[2] + d[2] * 0.001};
    const bool long_enough = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= kEps;
    const auto hits_block = [&](i32 x, i32 y, i32 z) {
        const BlockState s = w.block(x, y, z);
        if (LIKELY(s == kAir) || !long_enough) return false;
        for (const Box& b : boxes_of(s)) {
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

constexpr Box kFullBlock[1] = {{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}}};

}  // namespace

bool clip_hits(const Dimension& w, const double from[3], const double to[3]) {
    return clip_impl(w, from, to, [](BlockState s) {
        bool dynamic = false;
        return collision_boxes(s, dynamic);
    });
}

bool clip_hits_fall_resetting(const Dimension& w, const double from[3], const double to[3]) {
    // Asked every tick of everything that falls fast, and there is next to never such a block
    // near: the sections the line passes through say so without a block being read.
    if (!(w.holds(floor_i(std::min(from[0], to[0])) - 1, floor_i(std::min(from[1], to[1])) - 1,
                  floor_i(std::min(from[2], to[2])) - 1, floor_i(std::max(from[0], to[0])) + 1,
                  floor_i(std::max(from[1], to[1])) + 1, floor_i(std::max(from[2], to[2])) + 1) &
          Dimension::kFallBlocks))
        return false;
    return clip_impl(w, from, to, [](BlockState s) {
        const bool resets = physics::kMaterials[physics::kState[s] >> physics::kMaterialShift].flags & physics::kFallResetting;
        return std::span<const Box>(kFullBlock, resets ? 1u : 0u);
    });
}

// ---------------------------------------------------------------------------- explosions

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
                    // ExplosionDamageCalculator.getBlockExplosionResistance: air does not resist; a
                    // block resists as itself or as the fluid in it, whichever is more (a slab in
                    // water as water does). A fluid's resistance is its block's: mcgen checks.
                    float resistance = scratch.resistance[cell];
                    if (resistance == kUnknown) {
                        const BlockState state = w.block(bx, by, bz);
                        resistance = state == kAir || is_air(state) ? kNoResistance : block_info_of(state).explosion_resistance;
                        if (flags_of(state) & state::kHasFluid)
                            resistance = std::max(
                                resistance,
                                block_info_of(static_cast<BlockState>(state::fluid_legacy_of(state))).explosion_resistance);
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
