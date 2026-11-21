// SPDX-License-Identifier: GPL-3.0-or-later
// Boxes against the blocks of a dimension: how far something can move, what it stands on, and
// whether a line between two points is free.
//
// Ports of vanilla (26.3), each read from the jar:
//   BlockCollisions (which block shapes a box meets, in which order)   ->  all of the below
//   Shapes.joinIsNotEmpty, IndexMerger (does a shape overlap a box)    ->  the same
//   Shapes.collide / VoxelShape.collideX                               ->  collide()
//   Entity.collide, collectCandidateStepUpHeights (stepping up)        ->  collide()
//   CollisionGetter.noCollision                                        ->  no_collision()
//   CollisionGetter.findSupportingBlock                                ->  supporting_block()
//   BlockGetter.clip with ClipContext.Block.COLLIDER / FALLDAMAGE_RESETTING  ->  clip_hits*()
//   ServerExplosion.calculateExplodedPositions                         ->  blast_positions()
//
// A collision shape is what it is in vanilla - a grid of cells (gen::physics::kShapes), not a
// list of boxes - because the grid is observable: an entity is stopped by the cells in front of
// it, one cell at a time, and may step up to any of the grid's heights. The 1e-7 margins, the
// order of the comparisons and the order in which shapes are visited are vanilla's; movement
// that comes out of here is the movement vanilla computes, bit for bit (tests/unit/physics_test.cpp
// replays recordings of a vanilla server).
//
// Two implementations of everything: the one the server runs, with shortcuts for what nearly
// every block is (air, a full cube, one box), and `slow::` - vanilla's algorithms written out
// the long way, cell by cell. The unit tests hold the two against each other.
//
// Not here: entity colliders (boats, shulkers, happy ghasts), the world border, and shapes
// that depend on who asks (scaffolding, powder snow, a moving piston) - those count as empty;
// none can be placed.
#pragma once

#include <span>
#include <vector>

#include "world/dimension.hpp"

namespace world {

struct Aabb {
    double min[3], max[3];
};

// EntityDimensions.makeBoundingBox: the box of something `width` wide and `height` tall at (x, y, z).
ALWAYS_INLINE Aabb entity_box(double x, double y, double z, float width, float height) {
    const double w = static_cast<double>(width / 2.0f), h = static_cast<double>(height);
    return {{x - w, y, z - w}, {x + w, y + h, z + w}};
}
// AABB.expandTowards
ALWAYS_INLINE Aabb expand_towards(Aabb b, double dx, double dy, double dz) {
    if (dx < 0)
        b.min[0] += dx;
    else if (dx > 0)
        b.max[0] += dx;
    if (dy < 0)
        b.min[1] += dy;
    else if (dy > 0)
        b.max[1] += dy;
    if (dz < 0)
        b.min[2] += dz;
    else if (dz > 0)
        b.max[2] += dz;
    return b;
}
// AABB.move
ALWAYS_INLINE Aabb moved(const Aabb& b, double dx, double dy, double dz) {
    return {{b.min[0] + dx, b.min[1] + dy, b.min[2] + dz}, {b.max[0] + dx, b.max[1] + dy, b.max[2] + dz}};
}
// AABB.inflate (deflate is inflate by the negative)
ALWAYS_INLINE Aabb inflated(const Aabb& b, double dx, double dy, double dz) {
    return {{b.min[0] - dx, b.min[1] - dy, b.min[2] - dz}, {b.max[0] + dx, b.max[1] + dy, b.max[2] + dz}};
}
// AABB.intersects: touching is not intersecting.
ALWAYS_INLINE bool intersects(const Aabb& a, const Aabb& b) {
    return a.min[0] < b.max[0] && a.max[0] > b.min[0] && a.min[1] < b.max[1] && a.max[1] > b.min[1] && a.min[2] < b.max[2] &&
           a.max[2] > b.min[2];
}

// One collision shape a box meets, moved to its block (VoxelShape.move).
struct Collider {
    Aabb box;  // a shape of one cell: that cell. A grid: the block's own cube (its coordinates are in the shape)
    i32 x, y, z;
    u16 shape;  // 0 for a shape of one cell, else its index in gen::physics::kShapes
};
struct CollideScratch {  // reused between calls: no allocation once it has grown
    std::vector<Collider> colliders, step_colliders;
};

// `near`, in the three queries below: what the caller knows the sections around the box to hold
// (Dimension::holds over cells that include every cell the query reads - one block further
// than its box on every side), or kAskSections to have the query ask for itself. Whoever asks
// several things about one place asks the sections once: that is a chunk lookup and a cache
// line of the chunk each time, more than the rest of a query that then finds only air.
inline constexpr u32 kAskSections = ~0u;

// Entity.collide: the part of `move` the blocks leave to something whose box is `box`. Axis by
// axis in Direction.axisStepOrder - y first, then the longer of x and z - and, for what can
// step (`max_up_step`: Entity.maxUpStep(); `on_ground`: Entity.onGround() as the move begins),
// up onto what is in the way.
void collide(const Dimension& w, const Aabb& box, const double move[3], float max_up_step, bool on_ground, double out[3],
             CollideScratch& scratch, u32 near = kAskSections);

// CollisionGetter.noCollision(entity, box), blocks only: does no collision shape overlap `box`?
bool no_collision(const Dimension& w, const Aabb& box, u32 near = kAskSections);

// Level.findSupportingBlock(entity, box): of the blocks whose collision shape `box` overlaps,
// the one whose centre is nearest to (x, y, z); of several as near, the one that
// Vec3i.compareTo puts last (y, then z, then x). False if there is none.
bool supporting_block(const Dimension& w, const Aabb& box, double x, double y, double z, i32 out[3], u32 near = kAskSections);

// BlockGetter.clip(new ClipContext(from, to, COLLIDER, Fluid.NONE)).getType() != MISS: does the
// line from `from` to `to` meet the collision shape of a block?
bool clip_hits(const Dimension& w, const double from[3], const double to[3]);
// The same with ClipContext.Block.FALLDAMAGE_RESETTING: a block in #fall_damage_resetting
// (cobweb, what can be climbed) counts as a full cube, everything else as nothing.
bool clip_hits_fall_resetting(const Dimension& w, const double from[3], const double to[3]);

// The same answers the long way round (see top). For tests and for nothing else.
namespace slow {
void collide(const Dimension& w, const Aabb& box, const double move[3], float max_up_step, bool on_ground, double out[3]);
bool no_collision(const Dimension& w, const Aabb& box);
bool supporting_block(const Dimension& w, const Aabb& box, double x, double y, double z, i32 out[3]);
}  // namespace slow

// ServerExplosion.calculateExplodedPositions: the blocks an explosion of `radius` at (x, y, z)
// reaches - 1352 rays from the centre through the faces of a 16^3 cube, each with a strength of
// radius * (0.7 + 0.6 * strengths[i]) that every block on its way wears down by its explosion
// resistance. `strengths` are the rays' draws of RandomSource.nextFloat(), in order. `out` gets
// each position once, air included (vanilla counts it), in no order that means anything.
inline constexpr u32 kBlastRays = 16 * 16 * 16 - 14 * 14 * 14;
struct BlastScratch {               // reused between explosions
    std::vector<u64> reached;       // a bit per block of the cube the rays can get to
    std::vector<float> resistance;  // what each of those blocks resists with, once looked up
};
struct BlockAt {
    i32 x, y, z;
};
void blast_positions(const Dimension& w, double x, double y, double z, float radius, std::span<const float, kBlastRays> strengths,
                     BlastScratch& scratch, std::vector<BlockAt>& out);

}  // namespace world
