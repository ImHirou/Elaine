// A player's inventory and the menu over it: what is in which slot, what a click does, and
// what the client has been told.
//
// A port of vanilla's (26.3) Inventory, InventoryMenu and AbstractContainerMenu, with the
// ContainerSynchronizer of ServerPlayer folded in. Plain data and functions over it - no game
// state, no packets beyond its own synchronisation - so that all of it runs in a unit test.
// What a click does *outside* the menu (an item thrown into the world, the sound of armour
// going on) is returned as a list of Effects for the caller to act on, in order.
//
// Two numberings, both vanilla's:
//   inventory index   Inventory's own: 0..8 hotbar, 9..35 the rest, 36..39 feet/legs/chest/head,
//                     40 off hand (41 body, 42 saddle: no player can fill them). Used by
//                     set_player_inventory and by "swap with hotbar key N" clicks.
//   menu slot         InventoryMenu's: 0 crafting result, 1..4 crafting grid, 5..8 head/chest/
//                     legs/feet, 9..35 inventory, 36..44 hotbar, 45 off hand. Used by every
//                     container packet.
//
// Not here yet: recipes (the 2x2 grid holds items, the result slot stays empty), and what
// items do when clicked onto each other (bundles) - such a click is handled like any other,
// and the slot packets that follow put the client right.
#pragma once

#include <vector>

#include "base/buf.hpp"
#include "item/stack.hpp"

namespace server {

using item::ItemStack;

namespace inv {  // inventory indices
inline constexpr u32 kHotbarSize = 9, kItems = 36;
inline constexpr u32 kFeet = 36, kLegs = 37, kChest = 38, kHead = 39, kOffhand = 40, kBody = 41, kSaddle = 42;
inline constexpr u32 kSize = 43;
}  // namespace inv

namespace menu_slot {
inline constexpr i32 kResult = 0, kCraftFirst = 1, kCraftEnd = 5, kArmorFirst = 5, kArmorEnd = 9;
inline constexpr i32 kInvFirst = 9, kInvEnd = 36, kHotbarFirst = 36, kHotbarEnd = 45, kOffhand = 45, kCount = 46;
inline constexpr i32 kOutside = -999;  // AbstractContainerMenu.SLOT_CLICKED_OUTSIDE
}  // namespace menu_slot

// Something a menu operation asks of the world.
struct Effect {
    enum Kind : u8 {
        kDrop,           // LivingEntity.drop(stack, thrown, prediction)
        kEquipSound,     // LivingEntity.onEquipItem: armour or a shield went on
        kInventorySlot,  // Inventory.createInventoryUpdatePacket: tell the client this slot (set_player_inventory)
    };
    Kind kind;
    bool thrown = false;     // kDrop: thrown from the hand (the item remembers who threw it)
    bool predicted = false;  // kDrop: the client has already swung its arm (Prediction.PREDICTED)
    u8 slot = 0;             // kInventorySlot: inventory index
    u16 sound = 0;           // kEquipSound: sound_event id
    ItemStack stack;         // kDrop: what; kInventorySlot: what the slot held at that moment

    static Effect drop(ItemStack s, bool thrown_from_hand, bool client_predicted) {
        return {kDrop, thrown_from_hand, client_predicted, 0, 0, std::move(s)};
    }
    static Effect equip_sound(u32 sound_event) { return {kEquipSound, false, false, 0, static_cast<u16>(sound_event), {}}; }
    static Effect inventory_slot(u32 index, ItemStack now) {
        return {kInventorySlot, false, false, static_cast<u8>(index), 0, std::move(now)};
    }
};
using Effects = std::vector<Effect>;

// net.minecraft.world.entity.player.Inventory
struct Inventory {
    ItemStack items[inv::kSize];
    u8 selected = 0;  // hotbar slot in the main hand

    ItemStack& selected_item() { return items[selected]; }
    const ItemStack& selected_item() const { return items[selected]; }
    i32 free_slot() const;                                    // getFreeSlot: among 0..35, -1 if none
    i32 slot_with_remaining_space(const ItemStack& s) const;  // getSlotWithRemainingSpace
    i32 find_matching(const ItemStack& s) const;              // findSlotMatchingItem
    i32 suitable_hotbar_slot() const;                         // getSuitableHotbarSlot
    // add(slot, stack): into `slot`, or wherever it fits for slot -1. Takes what it placed off
    // `stack`. `infinite` is Player.hasInfiniteMaterials(): what does not fit is destroyed.
    bool add(i32 slot, ItemStack& stack, bool infinite);
    // placeItemBackInInventory(stack, Prediction.SERVER_ONLY): all of it, dropping what does not fit.
    void place_back(ItemStack stack, bool infinite, Effects& fx);
    void pick_slot(i32 slot);        // pickSlot
    void add_and_pick(ItemStack s);  // addAndPickItem
};

// The player's own menu (InventoryMenu, container id 0) and its synchronisation with the client.
class Menu {
  public:
    static constexpr u32 kContainerId = 0;

    struct Ctx {
        bool creative;  // hasInfiniteMaterials() / isCreative()
        Effects& fx;
    };

    Inventory inv;
    ItemStack craft[4];  // the 2x2 grid, row by row
    ItemStack carried;   // on the cursor

    // The stack in a menu slot.
    ItemStack& at(i32 slot);
    const ItemStack& at(i32 slot) const { return const_cast<Menu*>(this)->at(slot); }
    // The menu slot showing an inventory index, -1 for 41 and 42 (AbstractContainerMenu.findSlot).
    static i32 slot_of(u32 inventory_index);

    // ---- what packets do
    // AbstractContainerMenu.clicked. False where vanilla's code runs into an exception (a slot
    // index it never checked): the sender is to be disconnected.
    bool click(i32 slot, i32 button, u32 input, const Ctx& cx);
    // Slot.setByPlayer on a menu slot (handleSetCreativeModeSlot); the client has it already.
    void set_creative(i32 slot, ItemStack stack, const Ctx& cx);
    // InventoryMenu.removed: the menu was closed - the cursor and the crafting grid go back
    // into the inventory, or out into the world.
    void close(const Ctx& cx);
    // ServerPlayer.drop(all): one of, or all of, the stack in hand; the client predicted the loss.
    ItemStack take_from_hand(bool all);
    // handlePlayerAction SWAP_ITEM_WITH_OFFHAND
    void swap_hands(const Ctx& cx);
    // ServerGamePacketListenerImpl.tryPickItem: bring `stack` into the hand - by selecting or
    // moving the one already in the inventory, else (creative) by making it.
    void pick(const ItemStack& stack, bool creative);

    // ---- what the client knows (ContainerSynchronizer, RemoteSlot)
    // sendAllDataToRemote: container_set_content with everything.
    void send_all(base::ByteBuf& out, i32 threshold);
    // broadcastChanges: container_set_slot / set_cursor_item for what differs from what the
    // client is known to hold.
    void send_changes(base::ByteBuf& out, i32 threshold);
    // RemoteSlot.force / receive: the client now holds `stack` there - or, after a click it
    // predicted, something this server could not verify.
    void remote_is(i32 slot, const ItemStack& stack);
    void remote_is_unknown(i32 slot);
    void remote_carried_is(const ItemStack& stack);
    void remote_carried_is_unknown();
    u32 state_id() const { return state_id_; }

  private:
    static constexpr u64 kCarriedBit = 1ull << 63;

    // Slot behaviour, by menu slot.
    u32 slot_limit(i32 slot) const;                                                          // Slot.getMaxStackSize()
    u32 max_for(i32 slot, const ItemStack& s) const;                                         // Slot.getMaxStackSize(stack)
    bool may_place(i32 slot, const ItemStack& s) const;                                      // Slot.mayPlace
    bool may_pickup(i32 slot, const Ctx& cx) const;                                          // Slot.mayPickup
    void set_by_player(i32 slot, ItemStack s, const Ctx& cx);                                // Slot.setByPlayer(stack)
    bool try_remove(i32 slot, u32 amount, u32 max_amount, const Ctx& cx, ItemStack& taken);  // Slot.tryRemove
    ItemStack safe_take(i32 slot, u32 amount, u32 max_amount, const Ctx& cx);                // Slot.safeTake
    ItemStack safe_insert(i32 slot, ItemStack input, u32 amount, const Ctx& cx);             // Slot.safeInsert
    bool quick_replace(i32 slot, const ItemStack& s) const;  // canItemQuickReplace(slot, stack, true)
    void equip_hook(u32 equip_slot, const ItemStack& old, const ItemStack& now, const Ctx& cx);  // LivingEntity.onEquipItem

    bool move_to(ItemStack& stack, i32 first, i32 end, bool backwards, const Ctx& cx);  // moveItemStackTo
    ItemStack quick_move(i32 slot, const Ctx& cx);                                      // InventoryMenu.quickMoveStack
    void reset_quickcraft() {
        quickcraft_status_ = 0;
        quickcraft_slots_ = 0;
    }
    u32 next_state_id() { return state_id_ = (state_id_ + 1) & 32767; }  // incrementStateId

    ItemStack result_;  // slot 0; nothing ever puts a stack here, yet
    // What the client holds in each menu slot, where that is known: after the server told it, or
    // after a click whose predicted outcome it reported in full.
    ItemStack remote_[menu_slot::kCount];
    ItemStack remote_carried_;
    u64 remote_known_ = 0;  // bit = menu slot, kCarriedBit = the cursor
    u32 state_id_ = 0;
    // A drag over several slots (QUICK_CRAFT) arrives as start, one click per slot, end.
    u64 quickcraft_slots_ = 0;
    u8 quickcraft_status_ = 0;
    i8 quickcraft_type_ = -1;
};

}  // namespace server
