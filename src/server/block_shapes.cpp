// SPDX-License-Identifier: GPL-3.0-or-later
// Blocks whose state is their shape: the half a slab fills, the corner stairs turn, the arms of
// a fence, a pane and a wall, the way a chain, a lantern and an end rod point, the skin of a
// mushroom block. Each is asked two things - the state it is placed as, and the state it takes
// when a block beside it changes - by server/block_actions.cpp and server/block_updates.cpp.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   SlabBlock (getStateForPlacement, canBeReplaced)
//   StairBlock (getStateForPlacement, updateShape, getStairsShape, canTakeShape, isStairs)
//   FenceBlock (connectsTo, isSameFence), IronBarsBlock (attachsTo), and their getStateForPlacement / updateShape
//   WallBlock (connectsTo, getStateForPlacement, updateShape, topUpdate, sideUpdate, updateSides,
//   makeWallState, shouldRaisePost, isCovered)
//   FenceGateBlock.connectsToDirection, Block.isExceptionForConnection
//   LanternBlock, ChainBlock, EndRodBlock, GlazedTerracottaBlock, HugeMushroomBlock,
//   WaterloggedTransparentBlock / BarrierBlock / HeavyCoreBlock / MangroveRootsBlock
//
// A family has one layout of properties (props::slab, props::stairs...; mcgen holds every block
// of it to that), so a state is its block's first state plus what the properties add. What a
// neighbour is to these blocks beyond its sturdy faces is a byte a state: state::kConnect (a
// fence, a wall, a pane, a gate, something that takes no arms), and for the block on top of a
// wall state::kWallAbove (which of the wall's parts its underside covers).
//
// What is NOT here:
//   * water: a block placed into a source of water is `waterlogged` and stays so. Its
//     updateShape would ask for the water's tick (scheduleTick(pos, Fluids.WATER, ...)): there are
//     no fluid ticks, the water in it does not flow, and nothing but its item fills or empties it;
//   * a lead: FenceBlock.useWithoutItem ties the mobs a player leads to the fence. Nothing is on
//     a lead, so the click passes to the item in hand, as it does in vanilla then;
//   * a wall under a block whose collision shape depends on where it stands (bamboo, pointed
//     dripstone) reads the shape that block has in the middle of its cell.
#include "server/game.hpp"

namespace server {

using namespace proto;
using world::BlockInfo;
using world::BlockState;
namespace block_kind = proto::gen::block_kind;
namespace block_flag = world::block_flag;
namespace props = proto::gen::props;
namespace st = world::state;

namespace {

// Direction.getCounterClockWise, for the horizontal ones.
constexpr u32 kCounterClockwise[6] = {0, 0, world::kWest, world::kEast, world::kSouth, world::kNorth};

BlockPos step(BlockPos p, u32 dir) { return {p.x + world::kStepX[dir], p.y + world::kStepY[dir], p.z + world::kStepZ[dir]}; }
bool horizontal(u32 dir) { return dir >= world::kNorth; }
bool same_axis(u32 a, u32 b) { return (a >> 1) == (b >> 1); }

// A boolean property is (true, false): the state with it true is the lower one.
bool flag(BlockState s, const BlockInfo& info, u32 stride) { return (s - info.first_state) / stride % 2 == 0; }
BlockState with_flag(BlockState s, const BlockInfo& info, u32 stride, bool value) {
    if (flag(s, info, stride) == value) return s;
    return static_cast<BlockState>(value ? s - stride : s + stride);
}

// ---- SlabBlock: type (top, bottom, double), waterlogged
enum : u32 { kSlabTop, kSlabBottom, kSlabDouble };
u32 slab_type(BlockState s, const BlockInfo& info) { return (s - info.first_state) / props::slab::kTypeStride; }
BlockState slab_block(const BlockInfo& info, u32 type, bool waterlogged) {
    return static_cast<BlockState>(info.first_state + type * props::slab::kTypeStride + (waterlogged ? 0 : 1));
}

// ---- StairBlock: facing (north, south, west, east), half (top, bottom), shape, waterlogged
enum : u32 { kStraight, kInnerLeft, kInnerRight, kOuterLeft, kOuterRight };  // StairsShape
struct Stairs {
    u32 facing;  // a world::Direction
    bool top;
    u32 shape;
};
Stairs stairs_of(BlockState s, const BlockInfo& info) {
    const u32 off = s - info.first_state;
    return {world::kNorth + off / props::stairs::kFacingStride,
            off % props::stairs::kFacingStride / props::stairs::kHalfStride == 0,
            off % props::stairs::kHalfStride / props::stairs::kShapeStride};
}
BlockState with_stairs_shape(BlockState s, const BlockInfo& info, u32 shape) {
    const u32 now = (s - info.first_state) % props::stairs::kHalfStride / props::stairs::kShapeStride;
    return static_cast<BlockState>(s + (static_cast<i32>(shape) - static_cast<i32>(now)) *
                                           static_cast<i32>(props::stairs::kShapeStride));
}

// ---- CrossCollisionBlock (fences, bars, panes): east, north, south, waterlogged, west
constexpr u32 kCrossStride[6] = {
    0, 0, props::cross::kNorthStride, props::cross::kSouthStride, props::cross::kWestStride, props::cross::kEastStride};

// ---- WallBlock: east, north, south (none, low, tall), up, waterlogged, west
enum : u32 { kSideNone, kSideLow, kSideTall };  // WallSide
constexpr u32 kWallStride[6] = {
    0, 0, props::wall::kNorthStride, props::wall::kSouthStride, props::wall::kWestStride, props::wall::kEastStride};
u32 wall_side(BlockState s, const BlockInfo& info, u32 dir) { return (s - info.first_state) / kWallStride[dir] % 3; }

// ---- HugeMushroomBlock: down, east, north, south, up, west
constexpr u32 kMushroomStride[6] = {props::mushroom::kDownStride,  props::mushroom::kUpStride,   props::mushroom::kNorthStride,
                                    props::mushroom::kSouthStride, props::mushroom::kWestStride, props::mushroom::kEastStride};

// FenceBlock.connectsTo, IronBarsBlock.attachsTo, WallBlock.connectsTo: does a block of `kind`
// (in state `self`) reach out to `neighbour`, the block in direction `dir` of it? To a whole
// face - unless the block is one that takes no arms -, to its own kind, and each to some of
// the others.
bool connects(u32 kind, BlockState self, BlockState neighbour, u32 dir) {
    const u32 c = st::kConnect[neighbour];
    const bool face = !(c & st::kConnectException) && world::face_sturdy(neighbour, world::opposite(dir));
    // FenceGateBlock.connectsToDirection: a gate joins what stands in line with it.
    const bool gate = (c & (dir <= world::kSouth ? st::kConnectGateNS : st::kConnectGateWE)) != 0;
    switch (kind) {
        case block_kind::kFence:  // isSameFence: a fence, and wooden as this one is or is not
            return face || ((c & st::kConnectFence) && !((c ^ st::kConnect[self]) & st::kConnectWoodenFence)) || gate;
        case block_kind::kBars: return face || (c & (st::kConnectBars | st::kConnectWall));
        default: return (c & st::kConnectWall) || face || (c & st::kConnectBars) || gate;  // a wall
    }
}

// WallBlock.updateShape(level, state, topPos, topNeighbour, north, east, south, west): the sides
// that are connected are tall where the block on top covers them, low where it does not
// (updateSides, makeWallState); then the post (shouldRaisePost).
BlockState wall_block(const BlockInfo& info, bool waterlogged, BlockState above, const bool connected[6]) {
    const u32 a = st::kWallAbove[above];
    u32 side[6] = {};
    u32 state = info.first_state;
    for (u32 dir = world::kNorth; dir <= world::kEast; ++dir) {
        side[dir] = !connected[dir] ? kSideNone : ((a >> (dir - world::kNorth)) & 1) ? kSideTall : kSideLow;
        state += side[dir] * kWallStride[dir];
    }
    const bool n = side[world::kNorth] == kSideNone, s = side[world::kSouth] == kSideNone;
    const bool w = side[world::kWest] == kSideNone, e = side[world::kEast] == kSideNone;
    bool up;
    if (a & st::kWallPost) {  // a wall with a post stands on it
        up = true;
    } else if ((n && s && w && e) || n != s || w != e) {  // alone, an end or a corner
        up = true;
    } else if ((side[world::kNorth] == kSideTall && side[world::kSouth] == kSideTall) ||
               (side[world::kEast] == kSideTall && side[world::kWest] == kSideTall)) {
        up = false;
    } else {
        up = (a & (st::kPostOverride | st::kCoversPost)) != 0;
    }
    return static_cast<BlockState>(state + (up ? 0 : props::wall::kUpStride) +
                                   (waterlogged ? 0 : props::wall::kWaterloggedStride));
}

}  // namespace

// StairBlock.getStairsShape: stairs turn a corner where the stairs behind or in front of them
// stand across - unless the stairs on that side go on straight (canTakeShape).
u32 Game::stairs_shape(u32 facing, bool top, BlockPos pos) const {
    const auto stairs_at = [&](u32 dir, Stairs& out) {  // StairBlock.isStairs
        const BlockState s = state_for_client(step(pos, dir));
        const BlockInfo& info = world::block_info_of(s);
        if (info.kind != block_kind::kStairs) return false;
        out = stairs_of(s, info);
        return true;
    };
    const auto can_take_shape = [&](u32 dir) {
        Stairs n;
        return !stairs_at(dir, n) || n.facing != facing || n.top != top;
    };
    Stairs n;
    if (stairs_at(facing, n) && n.top == top && !same_axis(n.facing, facing) && can_take_shape(world::opposite(n.facing)))
        return n.facing == kCounterClockwise[facing] ? kOuterLeft : kOuterRight;
    if (stairs_at(world::opposite(facing), n) && n.top == top && !same_axis(n.facing, facing) && can_take_shape(n.facing))
        return n.facing == kCounterClockwise[facing] ? kInnerLeft : kInnerRight;
    return kStraight;
}

// SlabBlock.canBeReplaced(state, context): a slab makes room for the other half of itself - into
// the half that is free when the slab itself was clicked, always when it is where the block
// would go. `replacing_clicked`: BlockPlaceContext.replacingClickedOnBlock().
bool Game::slab_can_be_replaced(BlockState state, const ItemStack& held, const sb::UseItemOn& u, bool replacing_clicked) {
    const BlockInfo& info = world::block_info_of(state);
    const u32 type = slab_type(state, info);
    if (type == kSlabDouble || info.item != held.item()) return false;
    if (!replacing_clicked) return true;
    const bool upper = (static_cast<double>(u.pos.y) + static_cast<double>(u.cursor_y)) - static_cast<double>(u.pos.y) > 0.5;
    if (type == kSlabBottom) return u.direction == world::kUp || (upper && horizontal(u.direction));
    return u.direction == world::kDown || (!upper && horizontal(u.direction));
}

// Block.getStateForPlacement of the families above (kPlaceSlab ... kPlaceMushroom): the state a
// block is placed as at `at`; Section::kNoState: it cannot be.
BlockState Game::shaped_for_placement(u32 pi, const sb::UseItemOn& u, const BlockInfo& block, u32 kind, BlockPos at,
                                      bool replacing_clicked) const {
    const BlockState replaced = state_for_client(at);
    // FluidState.is(Fluids.WATER): a source of it, as a block or in one.
    const bool wet = world::holds_water_source(replaced);
    const u32 dry = wet ? 0 : 1;
    // BlockPlaceContext.getClickLocation().y - getClickedPos().getY() > 0.5: the upper half was clicked.
    const bool upper = (static_cast<double>(u.pos.y) + static_cast<double>(u.cursor_y)) - static_cast<double>(at.y) > 0.5;
    const bool top = u.direction == world::kDown || (u.direction != world::kUp && upper);
    const auto neighbour = [&](u32 dir) { return state_for_client(step(at, dir)); };
    switch (kind) {
        case block_flag::kPlaceSlab:  // the other half of the slab that is there, or the half that was clicked
            if (world::block_of(replaced) == world::block_of(block.first_state)) return slab_block(block, kSlabDouble, false);
            return slab_block(block, top ? kSlabTop : kSlabBottom, wet);
        case block_flag::kPlaceStairs: {  // the way the player looks, the half that was clicked
            const u32 facing = horizontal_direction(ents_.yaw[pi]);
            return static_cast<BlockState>(block.first_state + (facing - world::kNorth) * props::stairs::kFacingStride +
                                           (top ? 0 : props::stairs::kHalfStride) +
                                           stairs_shape(facing, top, at) * props::stairs::kShapeStride + dry);
        }
        case block_flag::kPlaceCross: {  // FenceBlock, IronBarsBlock
            u32 state = block.first_state + dry * props::cross::kWaterloggedStride;
            for (u32 dir = world::kNorth; dir <= world::kEast; ++dir)
                state += connects(block.kind, block.default_state, neighbour(dir), dir) ? 0 : kCrossStride[dir];
            return static_cast<BlockState>(state);
        }
        case block_flag::kPlaceWall: {
            bool connected[6] = {};
            for (u32 dir = world::kNorth; dir <= world::kEast; ++dir)
                connected[dir] = connects(block_kind::kWall, block.default_state, neighbour(dir), dir);
            return wall_block(block, wet, neighbour(world::kUp), connected);
        }
        case block_flag::kPlaceHorizontal:  // GlazedTerracottaBlock: facing the one who places it
            return static_cast<BlockState>(block.first_state +
                                           (world::opposite(horizontal_direction(ents_.yaw[pi])) - world::kNorth) *
                                               props::horizontal::kFacingStride);
        case block_flag::kPlaceChain: {  // RotatedPillarBlock's axis: x, y, z
            static constexpr u32 kAxis[6] = {1, 1, 2, 2, 0, 0};
            return static_cast<BlockState>(block.first_state + kAxis[u.direction] * props::chain::kAxisStride + dry);
        }
        case block_flag::kPlaceLantern: {  // hanging or standing, whichever it looks at first and can be
            u32 dirs[6];
            placement_directions(pi, u.direction, replacing_clicked, dirs);
            for (const u32 dir : dirs) {
                if (dir > world::kUp) continue;
                const BlockState state =
                    static_cast<BlockState>(block.first_state + (dir == world::kUp ? 0 : props::lantern::kHangingStride) + 1);
                if (can_survive(state, at)) return static_cast<BlockState>(state - (wet ? 1 : 0));
            }
            return world::Section::kNoState;
        }
        case block_flag::kPlaceEndRod: {  // out of the face that was clicked - or on, from a rod that points this way
            static constexpr u8 kIndex[6] = {5, 4, 0, 2, 3, 1};  // facing: north, east, south, west, up, down
            const BlockState behind = neighbour(world::opposite(u.direction));
            const bool same = world::block_of(behind) == world::block_of(block.first_state) &&
                              behind - block.first_state == kIndex[u.direction] * props::end_rod::kFacingStride;
            return static_cast<BlockState>(block.first_state + kIndex[same ? world::opposite(u.direction) : u.direction] *
                                                                   props::end_rod::kFacingStride);
        }
        case block_flag::kPlaceWaterlogged: return static_cast<BlockState>(block.first_state + dry);
        case block_flag::kPlaceMushroom: {  // skin on every side that is not more of the same block
            u32 state = block.first_state;
            for (u32 dir = 0; dir < 6; ++dir)
                state += world::block_of(neighbour(dir)) == world::block_of(block.first_state) ? kMushroomStride[dir] : 0;
            return static_cast<BlockState>(state);
        }
        default: return block.default_state;
    }
}

// BlockBehaviour.updateShape of the same families: what `state` at `pos` becomes when the block
// in direction `dir` of it is `neighbour`.
BlockState Game::shaped_update(BlockState state, const BlockInfo& info, BlockPos pos, u32 dir, BlockState neighbour) const {
    switch (info.kind) {
        case block_kind::kStairs: {  // a change beside them: the corner is looked at again
            if (!horizontal(dir)) return state;
            const Stairs s = stairs_of(state, info);
            return with_stairs_shape(state, info, stairs_shape(s.facing, s.top, pos));
        }
        case block_kind::kFence:
        case block_kind::kBars:  // the arm on that side
            return horizontal(dir) ? with_flag(state, info, kCrossStride[dir], connects(info.kind, state, neighbour, dir))
                                   : state;
        case block_kind::kWall: {
            if (dir == world::kDown) return state;
            bool connected[6] = {};
            for (u32 d = world::kNorth; d <= world::kEast; ++d) connected[d] = wall_side(state, info, d) != kSideNone;
            // topUpdate: the sides as they are, under another block. sideUpdate: one side anew.
            if (dir != world::kUp) connected[dir] = connects(block_kind::kWall, state, neighbour, dir);
            const BlockState above = dir == world::kUp ? neighbour : state_for_client({pos.x, pos.y + 1, pos.z});
            return wall_block(info, flag(state, info, props::wall::kWaterloggedStride), above, connected);
        }
        case block_kind::kLantern: {  // LanternBlock: what it hangs from or stands on went
            const bool hanging = flag(state, info, props::lantern::kHangingStride);
            return dir == (hanging ? world::kUp : world::kDown) && !can_survive(state, pos) ? world::kAir : state;
        }
        case block_kind::kMushroom:  // HugeMushroomBlock: no skin towards more of itself
            return world::block_of(neighbour) == world::block_of(state) ? with_flag(state, info, kMushroomStride[dir], false)
                                                                        : state;
        default: return state;
    }
}

}  // namespace server
