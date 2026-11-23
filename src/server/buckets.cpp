// SPDX-License-Identifier: GPL-3.0-or-later
// Buckets: emptied where their holder looks, and filled.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   BucketItem.use, emptyContents, playEmptySound, getFluidContext
//   Item.getPlayerPOVHitResult (the look: world::clip_outline is BlockGetter.clip)
//   ItemUtils.createFilledResult, Inventory.contains
// What a block does when a fluid is put into it or taken out of it is server/fluids.cpp
// (place_liquid, pickup_block: LiquidBlockContainer, BucketPickup).
//
// Everyone is creative here, so a bucket stays what it is: emptied it is still full, and one
// that is filled puts a full one beside it - if the inventory has none yet.
//
// What is NOT here:
//   * the bucket of a mob (MobBucketItem), of powder snow, of milk: other classes;
//   * a block of an imported world that holds water and is of a class that is not ported: it
//     takes none and gives none (server/fluids.cpp), though a bucket emptied at it sounds;
//   * particles: the smoke where water is poured out and cannot be (the nether has no water).
#include <cmath>

#include "phys/mth.hpp"
#include "server/game.hpp"
#include "world/collide.hpp"
#include "world/fluid.hpp"

#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
using world::BlockState;
namespace fluid = world::fluid;
namespace st = world::state;

namespace {

constexpr double kReach = 4.5 + 0.5;  // Player.blockInteractionRange: the attribute's 4.5, and half a block more in creative
// Avatar.POSES: the eyes of each pose (EntityDimensions.eyeHeight)
constexpr float kEyeStanding = 1.62f, kEyeCrouching = 1.27f, kEyeLow = 0.4f;

}  // namespace

// BucketItem.use: where the holder looks - as far as its hand reaches, a source of fluid
// stopping the look of an empty bucket and nothing but blocks that of a full one - the bucket is
// emptied into the block that was met if that holds water by itself, else against the side
// that was met; or, empty, filled from the block that was met.
Game::Used Game::use_bucket(u32 pi, const ItemStack& held) {
    const u32 item = held.item();
    const u32 content = item == fluid::kBucket[fluid::kWater]  ? fluid::kWater
                        : item == fluid::kBucket[fluid::kLava] ? fluid::kLava
                                                               : fluid::kEmpty;
    // Item.getPlayerPOVHitResult: from the eyes, along Entity.calculateViewVector(xRot, yRot).
    const u8 pose_now = ents_.pose[pi];
    const float eyes = pose_now == pose::kStanding ? kEyeStanding : pose_now == pose::kCrouching ? kEyeCrouching : kEyeLow;
    const double from[3] = {ents_.x[pi], ents_.y[pi] + static_cast<double>(eyes), ents_.z[pi]};
    const float real_x = ents_.pitch[pi] * 0.017453292f, real_y = -ents_.yaw[pi] * 0.017453292f;
    const float y_cos = phys::mth_cos(static_cast<double>(real_y)), y_sin = phys::mth_sin(static_cast<double>(real_y));
    const float x_cos = phys::mth_cos(static_cast<double>(real_x)), x_sin = phys::mth_sin(static_cast<double>(real_x));
    const double view[3] = {static_cast<double>(y_sin * x_cos), static_cast<double>(-x_sin), static_cast<double>(y_cos * x_cos)};
    const double to[3] = {from[0] + view[0] * kReach, from[1] + view[1] * kReach, from[2] + view[2] * kReach};
    const world::ClipHit hit =
        world::clip_outline(world_, from, to, content == fluid::kEmpty ? world::ClipFluid::kSourceOnly : world::ClipFluid::kNone);
    if (!hit.hit) return Used::kPass;
    const BlockPos pos{hit.x, hit.y, hit.z};
    const BlockPos beside{hit.x + world::kStepX[hit.face], hit.y + world::kStepY[hit.face], hit.z + world::kStepZ[hit.face]};
    // (Level.mayInteract, Player.mayUseItemAt: no spawn protection here, and everyone may build.)
    const BlockState clicked = world_.block(pos.x, pos.y, pos.z);
    if (content != fluid::kEmpty) {
        const BlockPos into = (st::kFluidFacts[clicked] & st::kFluidContainer) && content == fluid::kWater ? pos : beside;
        // ItemUtils.createFilledResult(stack, player, the stack itself): a creative player's bucket is as it was.
        // (FAIL where it went nowhere: to the server that is what PASS is - no arm swings.)
        return empty_bucket(pi, content, into, &hit) ? Used::kSuccess : Used::kPass;
    }
    // BucketPickup.pickupBlock: what the block gives - a source its whole self, a block that
    // holds water its water.
    const u32 taken = pickup_block(pos, clicked, true);
    if (!taken) return Used::kPass;
    // getPickupSound, as Player.playSound: where the player is, to everyone else.
    play_sound(taken == fluid::kBucket[fluid::kLava] ? gen::sound::kItemBucketFillLava : gen::sound::kItemBucketFill,
               cb::Sound::kSourcePlayers, ents_.x[pi], ents_.y[pi], ents_.z[pi], 1.0f, 1.0f, pi);
    // createFilledResult in creative: the hand keeps its bucket, and the full one is added if
    // there is none like it anywhere in the inventory (Inventory.contains).
    Inventory& inv = players_[pi].menu.inv;
    ItemStack full(taken, 1);
    bool has = false;
    for (const ItemStack& s : inv.items) has = has || (!s.empty() && item::same_item_same_components(s, full));
    if (!has) inv.add(-1, full, true);
    return Used::kSuccess;
}

// BucketItem.emptyContents: the fluid into the block at `pos` - if that is air, or gives way to
// it, or takes it (and its holder is not sneaking at it: then, as when it takes none, the
// fluid goes against the side that was clicked instead, `hit` saying which; the second try has
// no `hit`). True if it went anywhere - or would have, the place being a source already.
bool Game::empty_bucket(u32 pi, u32 content, BlockPos pos, const world::ClipHit* hit) {
    const BlockState state = world_.block(pos.x, pos.y, pos.z);
    const u32 facts = st::kFluidFacts[state];
    const bool may_replace = world::fluid_replaces(state);
    const bool container = (facts & st::kFluidContainer) != 0;
    // LiquidBlockContainer.canPlaceLiquid: a block that holds water takes a source of it (a
    // barrier from a player in creative mode: everyone here) - of the classes that are ported.
    const bool takes = container && content == fluid::kWater && (facts & (st::kTakesWater | st::kCreativeOnly));
    const bool place = may_replace || takes;
    const bool sneaking = players_[pi].shift;  // Entity.isShiftKeyDown
    if (!(world::is_air(state) || (place && (!sneaking || hit == nullptr)))) {
        if (!hit) return false;
        return empty_bucket(
            pi, content,
            {hit->x + world::kStepX[hit->face], hit->y + world::kStepY[hit->face], hit->z + world::kStepZ[hit->face]}, nullptr);
    }
    const double x = pos.x + 0.5, y = pos.y + 0.5, z = pos.z + 0.5;
    const auto emptied = [&]() {  // playEmptySound: at the block, to everyone but who did it
        play_sound(content == fluid::kLava ? gen::sound::kItemBucketEmptyLava : gen::sound::kItemBucketEmpty,
                   cb::Sound::kSourceBlocks, x, y, z, 1.0f, 1.0f, pi);
        return true;
    };
    if (world_.water_evaporates() && content == fluid::kWater) {
        // Where water does not last it is gone with a hiss (and smoke, which nothing here makes).
        play_sound(gen::sound::kBlockFireExtinguish, cb::Sound::kSourceBlocks, x, y, z, 0.5f,
                   2.6f + (random_float() - random_float()) * 0.8f, pi);
        return true;
    }
    if (container && content == fluid::kWater) {
        place_liquid(pos, state,
                     fluid_rules(fluid::kWaterFamily).source(false));  // (true whether it took any or was wet already)
        return emptied();
    }
    // What gives way and is no liquid breaks, with its drops (Level.destroyBlock) - then the source.
    if (may_replace && !(facts & st::kIsLiquid)) break_block(pos, true);
    // content.defaultFluidState().createLegacyBlock(): the fluid's block, a source
    const BlockState source = world::fluid_block(fluid_rules(world::fluid::kFamily[content]).source(false));
    if (set_block(pos, source) || world::fluid_source(world::fluid_of(state))) return emptied();
    return false;
}

}  // namespace server
