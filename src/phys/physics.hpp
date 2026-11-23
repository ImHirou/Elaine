// SPDX-License-Identifier: GPL-3.0-or-later
// Entity physics: where an entity that moves by itself is the next tick.
//
// A port of the parts of vanilla (26.3) that decide it, each read from the jar:
//   Entity.move, collide (world/collide.*), setOnGroundWithMovement / checkSupportingBlock,
//   getOnPos, getBlockSpeedFactor, checkFallDamage, restituteMovementAfterCollisions,
//   applyEffectsFromBlocks / checkInsideBlocks (BlockGetter.forEachBlockIntersectedBetween),
//   makeStuckInBlock, moveTowardsClosestSpace, push                      ->  move(), effects, push_impulse()
//   ItemEntity.tick                                                      ->  item_tick()
//   PrimedTnt.tick                                                       ->  tnt_tick()
//   FallingBlockEntity.tick (how it moves)                               ->  falling_move()
//   LivingEntity.aiStep / travel / travelInAir, with nobody steering     ->  living_tick()
//   WebBlock / HoneyBlock.entityInside, SlimeBlock.stepOn                ->  inside the effects
//   Entity.updateFluidInteraction, EntityFluidInteraction.update, CurrentAccumulator.applyTo,
//   FlowingFluid.getFlow (world/fluid.*); ItemEntity.setFluidMovement; LivingEntity.travelInFluid,
//   travelInWater, travelInLava, getFluidFallingAdjustedMovement, jumpOutOfFluid, and
//   travelInWater of the classes that have their own (Kind::water_travel)   ->  in a fluid
//
// The functions here are pure: a Motion (position and velocity, which the entity table holds
// in its columns), a Body (everything else Entity keeps about how it moves) and the blocks.
// No entity table, no packets, no clock - server/physics.cpp is the caller - so they run in a
// unit test against recordings of a vanilla server (tools/vanilla/physics_probe.py), and what
// comes out is compared bit for bit. To keep that true: no -ffast-math (the build has it off),
// no reordering of an expression, and where vanilla computes in float, float here. Things that
// look redundant are vanilla's and observable in the bits: `v + 0.0` (Vec3.add turns -0 into 0),
// two multiplications where one would do.
//
// In a fluid: every tick an entity looks at the cells its box is in (but a falling block, which
// never does), and what it finds - how high water and lava stand over its feet - decides how its
// tick goes on: an item floats up, a living entity sinks slowly, both are carried by the current.
// A section without fluid costs none of that: World::wet says so before a cell is read.
//
// What is NOT here:
//   * bubble columns; a float goal (a mob with a brain keeps itself up, here it sinks); swimming;
//     what stands on a fluid (the strider is not moved at all);
//   * what needs health: fall damage and its sound and particles, suffocation, the void's
//     damage - a living entity that falls out of the world is removed at once;
//   * sounds and vibrations of movement (footsteps), the honey block's slide sound and particles;
//   * entity colliders (boats, shulkers), the world border, pistons, powder snow, climbing;
//   * a brain: living_tick() is LivingEntity.travel with no input. Types that move in a way of
//     their own (gen_physics.OWN_MOVEMENT: what flies, swims, climbs or flops) are not moved.
#pragma once

#include "world/collide.hpp"

namespace phys {

// What physics has to know about the kind of entity it moves: EntityType, and what its class overrides.
struct Kind {
    float width, height;  // EntityDimensions
    float max_up_step;    // Entity.maxUpStep(): 0 for what is not alive
    double gravity;       // Entity.getDefaultGravity()
    float below_offset;   // getBlockPosBelowThatAffectsMyMovement: 0.500001 below the feet, an item 0.999999
    bool living;          // a LivingEntity
    // ---- in a fluid (gen::physics::kEntity has them by type)
    bool pushed_by_fluid = true;   // Entity.isPushedByFluid(): a current carries it
    u8 water_travel = 0;           // gen::physics::kWater*: LivingEntity.travelInWater, or its class's own
    float water_slow_down = 0.8f;  // LivingEntity.getWaterSlowDown()
    double jump_threshold = 0.0;   // Entity.getFluidJumpThreshold(): a fluid no deeper than this is shallow
    // What its MoveControl adds to its vertical velocity every tick, whatever it wants:
    double control_in_water = 0.0;    // ...while it is in water (a frog: up)
    double control_off_ground = 0.0;  // ...while it is not on the ground (a drowned: down)
};
inline constexpr Kind kItem{0.25f, 0.25f, 0.0f, 0.04, 0.999999f, false};          // EntityTypes.ITEM, ItemEntity
inline constexpr Kind kTnt{0.98f, 0.98f, 0.0f, 0.04, 0.500001f, false};           // EntityTypes.TNT, PrimedTnt
inline constexpr Kind kFallingBlock{0.98f, 0.98f, 0.0f, 0.04, 0.500001f, false};  // EntityTypes.FALLING_BLOCK, FallingBlockEntity
// A living entity of some size (EntityType) with the default attributes of its type.
constexpr Kind living_kind(float width, float height, float step_height, double gravity) {
    return {width, height, step_height, gravity, 0.500001f, true};
}
// ...of an entity type (gen::entity_type): its size, and how its class moves in a fluid.
Kind living_kind(u32 type);

// Position and velocity: Entity.position and deltaMovement. Loaded from the entity table's
// columns before a tick and stored back after it.
struct Motion {
    double pos[3];
    double vel[3];
};

inline constexpr u8 kSplashedBefore = 1, kSplashedAfter = 2;  // Body::splashed

// One leg of what an entity moved in a tick (Entity.Movement), kept for the blocks it passed through.
struct Movement {
    double from[3], to[3];
    bool ordered;  // axisDependentOriginalMovement is present: the leg is walked axis by axis...
    bool z_first;  // ...y first, then z before x (else x before z): Direction.axisStepOrder of what was asked for
};

// What else Entity keeps about how it moves. What every tick reads is in the first cache
// line, what a tick that moves writes in the second; the third is next to never touched.
struct Body {
    double fall_distance = 0;     // Entity.fallDistance
    double stuck[3] = {0, 0, 0};  // Entity.stuckSpeedMultiplier (a cobweb)
    i32 support[3] = {0, 0, 0};   // Entity.mainSupportingBlockPos...
    bool has_support = false;     // ...if present
    bool on_ground_no_blocks = false;
    bool on_ground = false;  // Entity.onGround()
    bool horizontal_collision = false, vertical_collision = false, vertical_collision_below = false;
    bool no_physics = false;     // Entity.noPhysics: an item inside a block
    bool no_gravity = false;     // Entity.isNoGravity()
    bool needs_sync = false;     // Entity.needsSync: set here; whoever tells the viewers takes it
    bool sync_position = false;  // Entity.syncPosition (a bounce): the same
    // ---- in a fluid: EntityFluidInteraction as of the entity's last look around it
    bool in_water = false;   // Entity.wasTouchingWater: isInWater()
    bool first_tick = true;  // Entity.firstTick: until its first base tick is over (never, for what has none: TNT)
    // Entity.doWaterSplashEffect ran this tick - set here; whoever makes the sound takes it - and
    // when: kSplashedBefore it moved (where the tick began), kSplashedAfter. With the sound's
    // volume, which is made of the velocity it had at that moment.
    u8 splashed = 0;
    float splash_volume = 0;
    double water_height = 0;  // getFluidHeight(FluidTags.WATER): how far over its feet water stands
    double lava_height = 0;   // ...and lava. isInLava() is "not its first tick, and this is not nothing"
    // ---- fire: lit by the lava it is in, put out by water. What burning does to something that
    // has health is its keeper's business (an item's health is the game's): told below.
    i32 fire_ticks = 0;         // Entity.remainingFireTicks
    bool fire_immune = false;   // Entity.fireImmune(): its type's - an item's also its stack's
    bool shows_fire = false;    // Entity.setSharedFlagOnFire, as its last base tick set it: what its viewers see
    bool burned = false;        // set here, as the next two: a tick of burning that hurts by one (Entity.baseTick)
    bool extinguished = false;  // its fire went out in a block (the hiss: playEntityOnFireExtinguishedSound)
    u16 lava_hurts = 0;         // Entity.lavaHurt: once for each cell of lava it was found in this tick
    // Entity.movementThisTick and finalMovementsThisTick. Vanilla keeps two lists; the first
    // only ever holds what move() recorded this tick and is emptied into the second before
    // anything reads either, so one array does: `moves` says that last[0] is this tick's and
    // not yet final, `finals` how many of `last` are the final movements.
    u8 moves = 0;
    u8 finals = 0;
    Movement last[2];
};

struct Scratch {  // reused between calls: no allocation once it has grown
    world::CollideScratch collide;
    std::vector<u64> along, told;  // block positions: seen on one leg of a movement, told this tick that an entity is in them
};
struct World {
    const world::Dimension& blocks;
    Scratch& scratch;
    // Set by each tick for its own length: what the sections hold where the entity is and
    // where this tick can take it (Dimension::holds) - asked once, for every query of the
    // tick - and whether that is nothing but air: then no block is read at all. (Bench
    // "physics", i5-9300H, ns a tick: an item falling through open air 138 -> 65; and asking
    // once rather than in each of a tick's three or four queries, each a chunk lookup and a
    // cache line of the chunk: an item on the ground 185 -> 156, one thrown 268 -> 228.)
    u32 near = world::kAskSections;
    bool open = false;
    // ...and whether there is fluid in them at all (LevelChunkSection.hasFluid: what vanilla asks
    // too before it looks at a cell - EntityFluidInteraction.hasFluidAndLoaded).
    bool wet = true;
    // Entity.updateFluidInteraction: how hard a current of lava pulls - the dimension's
    // (EnvironmentAttributes.FAST_LAVA: 0.007 there).
    double lava_flow_scale = 0.0023333333333333335;
};

// Entity.move(MoverType.SELF, delta).
void move(World& w, const Kind& k, Motion& m, Body& b, double dx, double dy, double dz);
// Entity.applyEffectsFromBlocks(): what the blocks it passed through this tick, and the one it
// stands on, do to it. `old_pos`: Entity.oldPosition(), where it was when the tick began.
void apply_effects_from_blocks(World& w, const Kind& k, Motion& m, Body& b, const double old_pos[3]);
// Entity.applyEffectsFromBlocksForLastMovements()
void apply_effects_from_last_movements(World& w, const Kind& k, Motion& m, Body& b);

enum class Fate : u8 { kStays, kGone };  // kGone: discarded (it fell out of the world)

// ItemEntity.tick: gravity - or the way up, in a fluid - the way out of a block it is stuck in,
// the move, friction. Not the stack's business (merging, age, pickup, what lava does to it). `tick_count`: Entity.tickCount, 1 in
// its first tick; `id`: Entity.getId() - an item at rest only moves every fourth tick, and which is its id's.
// `random_float(rng)`: RandomSource.nextFloat(), drawn only for an item stuck in a block.
Fate item_tick(World& w, Motion& m, Body& b, u32 tick_count, i32 id, float (*random_float)(void*), void* rng);
// PrimedTnt.tick: gravity, the move, drag, the hop off the ground, the fluids around it. Not the fuse.
void tnt_tick(World& w, Motion& m, Body& b);
// FallingBlockEntity.tick, the part before it looks where it is: gravity, the move, and what the
// blocks it passed do to it. What follows - landing, or the drag of the air - is the caller's
// (server/falling.cpp): it depends on the block the entity is.
void falling_move(World& w, Motion& m, Body& b);
// LivingEntity.tick for a living entity nobody steers: aiStep with no input - the velocity cut
// off below 0.003, travelInAir (move, then gravity and friction) or travelInFluid - and the
// blocks' effects. Not pushEntities: that takes the others (push_impulse below).
Fate living_tick(World& w, const Kind& k, Motion& m, Body& b);

// ---- at rest
//
// Most of what a world holds lies still, and what lies still does the same thing tick after
// tick for as long as no block near it changes and nothing pushes it: a living entity nothing
// at all (a tick leaves every bit of it as it was), an item a round of four ticks - gravity
// adds to its fall three times, the fourth it tries the ground, is stopped, and what is left of
// its sideways velocity (under 1e-7: not enough to move it) shrinks by the ground's friction.
// Knowing that, a tick of either costs next to nothing; see server/physics.cpp for who wakes
// them. What is at rest is decided by running the real ticks, never by reasoning about blocks.

// What the tick in which an item at rest tries the ground does to its sideways velocity:
// v = ((v + 0) * factor) * friction - the speed factor and the friction of what it lies on.
struct ItemRest {
    double factor = 1.0, friction = 1.0;
};
// Is this item at rest? `tick_count`: of the tick it had last. Takes its next four ticks on a
// copy and compares each with item_rest_tick(): true if they are the same to the bit and leave
// position and Body as they are - then so are all ticks after them.
bool item_rests(World& w, const Motion& m, const Body& b, u32 tick_count, i32 id, ItemRest& rest);
// item_tick() for an item at rest, on its velocity alone (nothing else of it changes, and no
// block is read). `phase`: (tickCount + id) & 3 of this tick. True: Entity.needsSync.
ALWAYS_INLINE bool item_rest_tick(double& vx, double& vy, double& vz, u32 phase, const ItemRest& rest) {
    if (phase == 0) {  // the move: stopped by the ground
        vx = (vx + 0.0) * rest.factor * rest.friction;
        vy = 0.0;
        vz = (vz + 0.0) * rest.factor * rest.friction;
        return true;  // its fall of 0.12 a tick is gone: more than a tenth, so viewers hear of it
    }
    if (phase == 1) vx = vx + 0.0, vz = vz + 0.0;  // a velocity that has just run out is -0 for one tick
    vy = vy + -kItem.gravity;
    return false;
}
// Two states that are the same to the bit (but for the sign of a zero in a velocity).
bool same_state(const Motion& a, const Body& x, const Motion& b, const Body& y);
// May setting the block at (x, y, z) matter to an entity of this size that is at rest here?
// Never false when it does: every block a tick at rest reads is within this reach, and so are
// the neighbours of those, whose shapes may change with a block set beside them.
bool block_in_reach(const Motion& m, float width, float height, i32 x, i32 y, i32 z);

// Entity.setOnGround(onGround): the flag, and the block it then stands on (a teleport does this).
void set_on_ground(World& w, float width, float height, const Motion& m, Body& b, bool on_ground);

// Entity.push(Entity), the arithmetic: called as a.push(b), a gets (-xa, -za) and b gets
// (xa, za) added to its velocity - if each is pushable. False when the two stand within
// a hundredth of a block of each other: then nothing is pushed.
bool push_impulse(const double a_pos[3], const double b_pos[3], double& xa, double& za);

}  // namespace phys
