// SPDX-License-Identifier: GPL-3.0-or-later
// What a player's packets do to its inventory, and what leaves the inventory for the world.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   ServerGamePacketListenerImpl.handleSetCreativeModeSlot / handleContainerClick /
//   handleContainerClose / handlePlayerAction (DROP_ITEM, DROP_ALL_ITEMS, SWAP_ITEM_WITH_OFFHAND) /
//   handlePunch
//   ServerPlayer.tick (the menu), ServerPlayer.drop, LivingEntity.drop / swing / onEquipItem /
//   detectEquipmentUpdates, LivingEntity.SwingState
// The menu itself - what a click does - is server/inventory.cpp.
//
// Everyone is in creative mode, which decides two things here: the creative inventory screen
// keeps its own copy and reports slots with set_creative_mode_slot (container_click comes from
// the other screens: a chest, a furnace, a crafting table), and what does not fit an inventory
// vanishes.
#include <cmath>

#include "base/log.hpp"
#include "phys/mth.hpp"
#include "server/game.hpp"

namespace server {

using namespace proto;
namespace equip = item::equip;

namespace {

// ServerGamePacketListenerImpl.dropSpamThrottler = new TickThrottler(20, 1480)
constexpr u32 kDropSpamStep = 20, kDropSpamLimit = 1480;
// LivingEntity.createItemStackToDrop: the item appears this far below the eyes.
constexpr double kDropBelowEyes = 0.30000001192092896;  // (double) 0.3f
// Avatar.POSES: eye heights. Entity.eyeHeight follows the pose.
constexpr float kEyeStanding = 1.62f, kEyeCrouching = 1.27f, kEyeLow = 0.4f;

float eye_height(u32 pose) { return pose == pose::kStanding ? kEyeStanding : pose == pose::kCrouching ? kEyeCrouching : kEyeLow; }

}  // namespace

// ---------------------------------------------------------------------------- packets

// A TickThrottler of bytes: what was sent is forgiven a little every tick.
bool Game::admit_heavy(Player& p, u32 bytes) {
    const u64 forgiven = (tick_ - p.heavy_inflow_tick) * kHeavyInflowPerTick;
    p.heavy_inflow -= static_cast<u32>(std::min<u64>(p.heavy_inflow, forgiven));
    p.heavy_inflow_tick = tick_;
    if (p.heavy_inflow + static_cast<u64>(bytes) > kHeavyInflowLimit) return false;
    p.heavy_inflow += bytes;
    return true;
}

u64 Game::carried_heavy(const Player& p) {
    u64 bytes = 0;
    for (const ItemStack& s : p.menu.inv.items) bytes += item::heavy_bytes(s);
    return bytes;
}

// handleSetCreativeModeSlot. The creative inventory lives on the client; this is how the server
// learns what is in a slot, or that a stack was thrown out of the window (a negative slot).
void Game::on_creative_slot(u32 pi, i32 slot, ItemStack stack, bool unsupported) {
    Player& p = players_[pi];
    const bool valid_slot = slot >= 1 && slot <= menu_slot::kOffhand;
    // More heavy stacks than a player may bring into being: refused like one that is not read.
    if (!unsupported && valid_slot && !may_carry(p, stack)) unsupported = true;
    if (unsupported) {
        // A stack with a component the server cannot vouch for (item/components.hpp): it is not
        // taken. The client shows it in the slot already, so the slot is sent back as it is here.
        LOG_DEBUG("%s: an item stack with components that are not supported yet was refused", p.name.c_str());
        if (valid_slot) {
            p.menu.own_remote_is_unknown(slot);
            p.menu.send_own_changes(out(pi), p.threshold);
        }
        return;
    }
    if (!stack.empty() && stack.count() > stack.max_stack()) return;  // "validData"
    if (valid_slot) {
        p.menu.set_creative(slot, std::move(stack), menu_ctx(pi));
        apply_effects(pi);
        p.menu.send_own_changes(out(pi), p.threshold);  // broadcastChanges: nothing, unless something else was pending
        dirty_[pi] |= kEquipDirty;
    } else if (slot < 0) {
        p.drop_spam -= static_cast<u32>(std::min<u64>(p.drop_spam, tick_ - p.drop_spam_tick));  // TickThrottler.tick, caught up
        p.drop_spam_tick = tick_;
        if (p.drop_spam >= kDropSpamLimit) {
            LOG_DEBUG("%s was dropping items too fast in creative mode, ignoring", p.name.c_str());
            return;
        }
        p.drop_spam += kDropSpamStep;
        drop(pi, std::move(stack), true, true);
    }
}

// handleContainerClick
void Game::on_container_click(u32 pi, const sb::ContainerClick& c) {
    Player& p = players_[pi];
    Menu& menu = p.menu;
    if (c.container_id != menu.container_id()) return;  // not the menu that is open
    // containerMenu.stillValid: a click on a chest that is out of reach by now does nothing.
    if (menu.kind() != Menu::Kind::kInventory && !menu_still_valid(pi)) return;
    // AbstractContainerMenu.isValidSlotIndex: -1 and -999 pass by name, the other negatives by oversight.
    const i32 slots = menu.slot_count();
    if (c.slot >= slots) return;
    // What HashedStack.STREAM_CODEC throws on: ids that are in no registry.
    const auto decodable = [](const sb::HashedStack& h) {
        return !h.present || (h.item < world::item_count() && h.highest_type < item::component::kCount);
    };
    bool ok = decodable(c.carried);
    for (u32 i = 0; i < c.changed_count; ++i) ok &= decodable(c.changed[i].stack);
    const bool resync = c.state_id != menu.state_id();  // the client clicked on something older than what it was sent
    if (ok) ok = menu.click(c.slot, c.button, c.input, menu_ctx(pi));
    if (!ok) {
        effects_.clear();
        kick(pi, "Invalid container click");
        return;
    }
    apply_effects(pi);
    // Container.setChanged: what a chest or a furnace holds may have changed - their chunks are
    // to be saved again.
    if (menu.kind() == Menu::Kind::kChest) {
        for (const u32 chest : p.open_data)
            if (chest != kNone) world_.mark_dirty(chests_[chest].x, chests_[chest].z);
    } else if (menu.kind() == Menu::Kind::kFurnace) {
        world_.mark_dirty(p.open_pos.x, p.open_pos.z);
    }

    // The client says what its own prediction left in the slots it touched. Where that can be
    // checked against a stack (no components: their hashes are not computed here yet) and it
    // agrees, nothing has to be sent; everything else the client is told again.
    const auto known = [](const sb::HashedStack& h, ItemStack& s) {
        s = {};
        if (!h.present) return true;
        if (h.components || h.item == 0 || h.count < 1 || h.count > static_cast<i32>(item::kMaxCount)) return false;
        s = ItemStack(h.item, static_cast<u32>(h.count));
        return true;
    };
    ItemStack believed;
    for (u32 i = 0; i < c.changed_count; ++i) {
        const i32 slot = c.changed[i].slot;
        if (slot < 0 || slot >= slots) continue;  // setRemoteSlotUnsafe
        if (known(c.changed[i].stack, believed))
            menu.remote_is(slot, believed);
        else
            menu.remote_is_unknown(slot);
    }
    if (known(c.carried, believed))
        menu.remote_carried_is(believed);
    else
        menu.remote_carried_is_unknown();
    if (resync)
        menu.send_all(out(pi), p.threshold, containers(p));  // broadcastFullState
    else
        menu.send_changes(out(pi), p.threshold, containers(p));
    dirty_[pi] |= kEquipDirty;
}

// handleContainerClose -> ServerPlayer.doCloseContainer. Whatever menu is open closes (the
// packet's container id is not looked at): what was on the cursor and in a crafting grid goes
// back into the inventory, and the player's own menu is the open one again.
void Game::on_container_close(u32 pi) { close_menu(pi, false); }

// handlePlayerAction DROP_ITEM / DROP_ALL_ITEMS -> ServerPlayer.drop(all): the Q key.
void Game::on_drop_key(u32 pi, bool all) {
    drop(pi, players_[pi].menu.take_from_hand(all), true, true);
    dirty_[pi] |= kEquipDirty;
}

// handlePlayerAction SWAP_ITEM_WITH_OFFHAND: the F key. The client waits to be told.
void Game::on_swap_hands(u32 pi) {
    players_[pi].menu.swap_hands(menu_ctx(pi));
    apply_effects(pi);
    touch_inventory(pi);
}

// ---------------------------------------------------------------------------- per tick

// ServerPlayer.tick: containerMenu.broadcastChanges() - whatever changed in a menu since its
// client was last told (an item picked up, hands swapped) goes out now, ahead of the player's
// own tick. Vanilla compares every slot of every player every tick; here only the menus
// something has touched.
//
// A menu on a chest or a furnace is looked at every tick while it is open - somebody else may
// have a hand in the same chest, and a furnace burns - and closed when its block is gone or
// out of reach (containerMenu.stillValid).
void Game::sync_menus() {
    const u32 n = static_cast<u32>(players_.size());
    for (u32 pi = 0; pi < n; ++pi) {
        if (LIKELY(!(dirty_[pi] & kMenuDirty))) continue;
        dirty_[pi] &= static_cast<u8>(~kMenuDirty);
        Player& p = players_[pi];
        p.menu.send_changes(out(pi), p.threshold, containers(p));
        if (p.menu.kind() == Menu::Kind::kInventory) continue;
        if (menu_still_valid(pi))
            dirty_[pi] |= kMenuDirty;
        else
            close_menu(pi, true);
    }
}

// LivingEntity.detectEquipmentUpdates, for a player: what is in its hands and on its body.
void Game::show_equipment(u32 pi) {
    const Inventory& inv = players_[pi].menu.inv;
    ItemStack now[equip::kCount];
    now[equip::kMainhand] = inv.selected_item();  // PlayerEquipment: the main hand is the selected hotbar slot
    now[equip::kOffhand] = inv.items[inv::kOffhand];
    now[equip::kFeet] = inv.items[inv::kFeet];
    now[equip::kLegs] = inv.items[inv::kLegs];
    now[equip::kChest] = inv.items[inv::kChest];
    now[equip::kHead] = inv.items[inv::kHead];
    now[equip::kBody] = inv.items[inv::kBody];
    now[equip::kSaddle] = inv.items[inv::kSaddle];
    tracker_.show_equipment(pi, now, out_.data());
}

// ---------------------------------------------------------------------------- into the world

void Game::apply_effects(u32 pi) {
    // By index: nothing below adds to the list, but drop() is far from here.
    for (usize i = 0; i < effects_.size(); ++i) {
        Effect& fx = effects_[i];
        switch (fx.kind) {
            case Effect::kDrop: drop(pi, std::move(fx.stack), fx.thrown, fx.predicted); break;
            case Effect::kEquipSound: play_sound(fx.value, ents_.x[pi], ents_.y[pi], ents_.z[pi]); break;
            case Effect::kInventorySlot: send(pi, cb::SetPlayerInventory{fx.slot, item::view(fx.stack)}); break;
            case Effect::kSetSlot: send(pi, cb::ContainerSetSlot{fx.slot, fx.value, 0, item::view(fx.stack)}); break;
        }
    }
    effects_.clear();
}

// LivingEntity.drop(stack, thrownFromHand, prediction): the arm swings (SwingAnimation.DEFAULT,
// whatever is in the hand) and the stack becomes an item entity. `predicted`: the client swung
// its own arm already and is not told to.
//
// The item is thrown the way the player looks (createItemStackToDrop).
void Game::drop(u32 pi, ItemStack stack, bool thrown, bool predicted) {
    if (stack.empty()) return;
    swing(pi, cb::SwingAnimation::kMainHand, {}, !predicted);
    const double y = ents_.y[pi] + static_cast<double>(eye_height(ents_.pose[pi])) - kDropBelowEyes;
    double v[3];
    throw_velocity(pi, v);
    spawn_item(std::move(stack), ents_.x[pi], y, ents_.z[pi], v[0], v[1], v[2], 40);
    (void)thrown;  // ItemEntity.thrower: only advancement triggers ask who threw it
}

// LivingEntity.createItemStackToDrop, the velocity of what is not dropped "randomly": three
// tenths of a block a tick along the look, a little up, and a little of anything. Vanilla's
// sines here are Mth's table, and the sum is a float.
void Game::throw_velocity(u32 pi, double v[3]) {
    const float sin_x = phys::mth_sin(static_cast<double>(ents_.pitch[pi] * 0.017453292f));
    const float cos_x = phys::mth_cos(static_cast<double>(ents_.pitch[pi] * 0.017453292f));
    const float sin_y = phys::mth_sin(static_cast<double>(ents_.yaw[pi] * 0.017453292f));
    const float cos_y = phys::mth_cos(static_cast<double>(ents_.yaw[pi] * 0.017453292f));
    const float dir = random_float() * 6.2831855f;
    const float spread = 0.02f * random_float();
    const float lift = random_float() - random_float();
    v[0] = static_cast<double>(-sin_y * cos_x * 0.3f) + std::cos(static_cast<double>(dir)) * static_cast<double>(spread);
    v[1] = static_cast<double>(-sin_x * 0.3f + 0.1f + lift * 0.1f);
    v[2] = static_cast<double>(cos_y * cos_x * 0.3f) + std::sin(static_cast<double>(dir)) * static_cast<double>(spread);
}

// Player.createItemStackToDrop + ItemEntity.makeFakeItem: an item that is only there to be
// seen flying into the player (/give) - never picked up, never merged, and at the end of its
// life already: its first tick, later in this one, discards it.
void Game::fake_item(u32 pi, ItemStack stack) {
    const double y = ents_.y[pi] + static_cast<double>(eye_height(ents_.pose[pi])) - kDropBelowEyes;
    double v[3];
    throw_velocity(pi, v);
    spawn_item(std::move(stack), ents_.x[pi], y, ents_.z[pi], v[0], v[1], v[2], kNever, 5999);
}

// handlePunch: the left mouse button, whatever it hit - the air, a block (the block itself is
// player_action's business), an entity (`attack` came first). The client swung its own arm; the
// others are shown the attack animation of what the main hand holds.
//
// Vanilla also restarts the attack cooldown here (resetAttackStrengthTicker). Nothing can be
// hit yet, so there is no cooldown to keep.
void Game::on_punch(u32 pi) {
    swing(pi, cb::SwingAnimation::kMainHand, players_[pi].menu.inv.selected_item().attack_swing(), false);
}

// LivingEntity.swing(hand, animation, sendToSwingingEntity): everyone who sees the player is
// told that its arm swings - and the player's own client too, when it did not swing by itself.
// False, and nothing sent, while a swing is in its first half (SwingState.startIfAble).
//
// swingState.tick() runs once per player tick, so its counter when a packet is handled is the
// number of ticks since the swing began: 0 for one begun by an earlier packet of the same tick,
// which is begun again (two packets, as in vanilla). The duration is the animation's own:
// nothing shortens or stretches it yet (haste, mining fatigue: getModifiedSwingDuration).
bool Game::swing(u32 pi, u32 hand, proto::Swing how, bool to_self) {
    Player& p = players_[pi];
    const u64 since = tick_ - p.swing_tick;
    if (p.swing_tick != kNever && since != 0 && since <= p.swing_len / 2u) return false;
    p.swing_tick = tick_;
    p.swing_len = how.ticks;
    scratch_.clear();
    write_packet(scratch_, s_.compression_threshold, cb::SwingAnimation{ents_.net_id(pi), hand, how});
    tracker_.broadcast(pi, {scratch_.data, scratch_.len}, out_.data());
    if (to_self) out(pi).append(scratch_.data, scratch_.len);
    return true;
}

// ServerLevel.playSeededSound(null, x, y, z, sound, PLAYERS, 1, 1, seed) -> PlayerList.broadcast:
// everyone closer than the sound's range, whoever caused it included.
void Game::play_sound(u32 sound, double x, double y, double z) {
    play_sound(sound, cb::Sound::kSourcePlayers, x, y, z, 1.0f, 1.0f);
}

}  // namespace server
