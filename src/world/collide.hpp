// Boxes against the blocks of a dimension: how far something can move, what it stands on, and
// whether a line between two points is free.
//
// Ports of the parts of vanilla (26.3) that an entity without a step height needs:
//   Entity.collide / collideBoundingBox / collideWithShapes  ->  collide()
//   BlockCollisions (which block shapes a box can meet)      ->  both
//   Shapes.collide / VoxelShape.collideX                     ->  collide(), per box of a shape
//   Level.findSupportingBlock                                ->  supporting_block()
//   BlockGetter.clip with ClipContext.Block.COLLIDER         ->  clip_hits()
//   ServerExplosion.calculateExplodedPositions               ->  blast_positions()
//
// A block's shape is its list of boxes (world::collision_boxes), where vanilla has a voxel grid
// built from the same boxes; the 1e-7 margins are vanilla's. The two agree except for a box
// that already overlaps a shape of several boxes, where vanilla stops at the first cell of the
// shape and this looks at every box. States whose shape depends on who asks (scaffolding,
// powder snow) count as empty: none can be placed yet.
//
// Not here: entity colliders (boats, shulkers), the world border, stepping up.
#pragma once

#include <span>
#include <vector>

#include "world/dimension.hpp"

namespace world {

struct Aabb {
    double min[3], max[3];
};

// Entity.collide for maxUpStep() == 0: the part of `move` the blocks leave to a `box`, axis by
// axis in Direction.axisStepOrder - y first, then the longer of x and z. `scratch` is reused
// between calls (no allocation once it has grown).
void collide(const Dimension& w, const Aabb& box, const double move[3], double out[3], std::vector<Aabb>& scratch);

// Level.findSupportingBlock(entity, box): of the blocks whose collision shape `box` touches, the
// one whose centre is nearest to (x, y, z). False if there is none.
bool supporting_block(const Dimension& w, const Aabb& box, double x, double y, double z, i32 out[3]);

// BlockGetter.clip(new ClipContext(from, to, COLLIDER, Fluid.NONE)).getType() != MISS: does the
// line from `from` to `to` meet the collision shape of a block?
bool clip_hits(const Dimension& w, const double from[3], const double to[3]);

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
