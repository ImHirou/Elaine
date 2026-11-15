// A player's inventory and the menus over it: what is in which slot, what a click does, and
// what the client has been told.
//
// A port of vanilla's (26.3) Inventory, AbstractContainerMenu, and the menus the server opens -
// InventoryMenu (the player's own), ChestMenu, FurnaceMenu, CraftingMenu - with their slot
// classes (ResultSlot, FurnaceFuelSlot, FurnaceResultSlot, ArmorSlot) and the
// ContainerSynchronizer of ServerPlayer folded in. Plain data and functions over it - no game
// state, no packets beyond its own synchronisation - so that all of it runs in a unit test.
// What a click does *outside* the menu (an item thrown into the world, the sound of armour
// going on, a crafting result shown at once) is returned as a list of Effects for the caller to
// act on, in order.
//
// A player has two menus at any time, as in vanilla: its own (container id 0, always there) and
// the one that is open - which is its own unless a chest, a furnace or a crafting table was
// clicked. One Menu object holds both: the inventory they share, each one's cursor and
// crafting grid, and what the client is known to hold of each.
//
// Numberings, all vanilla's:
//   inventory index   Inventory's own: 0..8 hotbar, 9..35 the rest, 36..39 feet/legs/chest/head,
//                     40 off hand (41 body, 42 saddle: no player can fill them). Used by
//                     set_player_inventory and by "swap with hotbar key N" clicks.
//   menu slot         by menu, used by every container packet:
//     own (46)        0 crafting result, 1..4 crafting grid, 5..8 head/chest/legs/feet,
//                     9..35 inventory, 36..44 hotbar, 45 off hand
//     chest (63, 90)  0..26 (0..53 for a double one) the chest, then inventory, then hotbar
//     furnace (39)    0 input, 1 fuel, 2 result, 3..29 inventory, 30..38 hotbar
//     crafting (46)   0 result, 1..9 the grid, 10..36 inventory, 37..45 hotbar
//
// Not here yet: what items do when clicked onto each other (bundles) - such a click is handled
// like any other, and the slot packets that follow put the client right; the recipe book
// (place_recipe); the experience a furnace's result gives.
#pragma once

#include <vector>

#include "base/buf.hpp"
#include "item/stack.hpp"
#include "server/block_entity.hpp"

namespace server {

namespace inv {  // inventory indices
inline constexpr u32 kHotbarSize = 9, kItems = 36;
inline constexpr u32 kFeet = 36, kLegs = 37, kChest = 38, kHead = 39, kOffhand = 40, kBody = 41, kSaddle = 42;
inline constexpr u32 kSize = 43;
}  // namespace inv

namespace menu_slot {  // of the player's own menu
inline constexpr i32 kResult = 0, kCraftFirst = 1, kCraftEnd = 5, kArmorFirst = 5, kArmorEnd = 9;
inline constexpr i32 kInvFirst = 9, kInvEnd = 36, kHotbarFirst = 36, kHotbarEnd = 45, kOffhand = 45, kCount = 46;
inline constexpr i32 kOutside = -999;  // AbstractContainerMenu.SLOT_CLICKED_OUTSIDE
inline constexpr i32 kMax = 90;        // the largest menu: a double chest, 54 + 36
}  // namespace menu_slot

// Something a menu operation asks of the world.
struct Effect {
    enum Kind : u8 {
        kDrop,           // LivingEntity.drop(stack, thrown, prediction)
        kEquipSound,     // LivingEntity.onEquipItem: armour or a shield went on
        kInventorySlot,  // Inventory.createInventoryUpdatePacket: tell the client this slot (set_player_inventory)
        kSetSlot,        // CraftingMenu.slotChangedCraftingGrid: the result slot, sent the moment the grid changes
    };
    Kind kind;
    bool thrown = false;     // kDrop: thrown from the hand (the item remembers who threw it)
    bool predicted = false;  // kDrop: the client has already swung its arm (Prediction.PREDICTED)
    u8 slot = 0;             // kInventorySlot: inventory index. kSetSlot: container id
    u16 value = 0;           // kEquipSound: sound_event id. kSetSlot: the menu's new state id
    ItemStack stack;         // kDrop: what; kInventorySlot, kSetSlot: what the slot holds at that moment

    static Effect drop(ItemStack s, bool thrown_from_hand, bool client_predicted) {
        return {kDrop, thrown_from_hand, client_predicted, 0, 0, std::move(s)};
    }
    static Effect equip_sound(u32 sound_event) { return {kEquipSound, false, false, 0, static_cast<u16>(sound_event), {}}; }
    static Effect inventory_slot(u32 index, ItemStack now) {
        return {kInventorySlot, false, false, static_cast<u8>(index), 0, std::move(now)};
    }
    static Effect set_slot(u32 container, u32 state, ItemStack now) {
        return {kSetSlot, false, false, static_cast<u8>(container), static_cast<u16>(state), std::move(now)};
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

// The containers behind the menu that is open - resolved by the caller for each operation,
// never kept: a chest's stacks live in a store that grows.
struct Containers {
    ItemStack* chest[2] = {nullptr, nullptr};  // ChestMenu: 27 stacks each; [1] is the second half of a double chest
    Furnace* furnace = nullptr;                // FurnaceMenu
};

class Menu {
  public:
    static constexpr u32 kContainerId = 0;  // of the player's own menu

    // Which menu is open (AbstractContainerMenu's subclasses).
    enum class Kind : u8 { kInventory, kChest, kFurnace, kCrafting };

    struct Ctx {
        bool creative;  // hasInfiniteMaterials() / isCreative()
        Effects& fx;
        Containers c = {};
    };

    Inventory inv;
    ItemStack craft[4];  // the 2x2 grid of the player's own menu, row by row
    ItemStack carried;   // on the cursor of the menu that is open

    Kind kind() const { return kind_; }
    u32 container_id() const { return kind_ == Kind::kInventory ? kContainerId : container_id_; }
    i32 slot_count() const;
    u32 chest_rows() const { return rows_; }

    // The stack in a slot of the open menu. For a slot of a chest or a furnace: through the
    // Containers the last operation was given, so only while those still stand.
    ItemStack& at(i32 slot);
    const ItemStack& at(i32 slot) const { return const_cast<Menu*>(this)->at(slot); }
    // The slot of the player's own menu showing an inventory index, -1 for 41 and 42
    // (AbstractContainerMenu.findSlot).
    static i32 slot_of(u32 inventory_index);

    // ---- opening and closing
    // ServerPlayer.openMenu's new menu: a ChestMenu of `rows` rows, a FurnaceMenu, a
    // CraftingMenu. The player's own menu must be the one open (close() first).
    void open(Kind kind, u32 container_id, u32 rows = 0);
    // ServerPlayer.doCloseContainer: AbstractContainerMenu.removed of the open menu - the cursor
    // (and a crafting grid) go back into the inventory, or out into the world - after which the
    // player's own menu is the open one again and knows what the client holds of the inventory.
    void close(const Ctx& cx);

    // ---- what packets do
    // AbstractContainerMenu.clicked. False where vanilla's code runs into an exception (a slot
    // index it never checked): the sender is to be disconnected.
    bool click(i32 slot, i32 button, u32 input, const Ctx& cx);
    // handleSetCreativeModeSlot: Slot.setByPlayer on a slot of the player's own menu, whichever
    // menu is open; the client has the stack already.
    void set_creative(i32 slot, ItemStack stack, const Ctx& cx);
    // ServerPlayer.drop(all): one of, or all of, the stack in hand; the client predicted the loss.
    ItemStack take_from_hand(bool all);
    // handlePlayerAction SWAP_ITEM_WITH_OFFHAND
    void swap_hands(const Ctx& cx);
    // ServerGamePacketListenerImpl.tryPickItem: bring `stack` into the hand - by selecting or
    // moving the one already in the inventory, else (creative) by making it.
    void pick(const ItemStack& stack, bool creative);

    // ---- what the client knows (ContainerSynchronizer, RemoteSlot), of the open menu
    // sendAllDataToRemote: container_set_content with everything, then the data values.
    void send_all(base::ByteBuf& out, i32 threshold, const Containers& c = {});
    // broadcastChanges: container_set_slot / set_cursor_item / container_set_data for what
    // differs from what the client is known to hold.
    void send_changes(base::ByteBuf& out, i32 threshold, const Containers& c = {});
    // The same for the player's own menu while another one is open (handleSetCreativeModeSlot).
    void send_own_changes(base::ByteBuf& out, i32 threshold);
    // RemoteSlot.force / receive: the client now holds `stack` there - or, after a click it
    // predicted, something this server could not verify.
    void remote_is(i32 slot, const ItemStack& stack);
    void remote_is_unknown(i32 slot);
    void remote_carried_is(const ItemStack& stack);
    void remote_carried_is_unknown();
    void own_remote_is_unknown(i32 slot);  // a slot of the player's own menu
    u32 state_id() const { return sync().state_id; }

  private:
    // The kinds of slot there are (Slot and its subclasses).
    enum class SlotKind : u8 { kResult, kCraft, kArmor, kPlain, kOffhand, kInput, kFuel, kOutput };

    // What one AbstractContainerMenu has of its own: what its client holds, where it is in a
    // drag.
    struct Sync {
        ItemStack remote[menu_slot::kMax];
        ItemStack remote_carried;
        u64 known[2] = {0, 0};  // bit = slot: remote[slot] is what the client holds
        bool carried_known = false;
        u32 state_id = 0;
        i32 remote_data[Furnace::kDataCount] = {0, 0, 0, 0};
        // A drag over several slots (QUICK_CRAFT) arrives as start, one click per slot, end.
        u64 quickcraft_slots[2] = {0, 0};
        u8 quickcraft_status = 0;
        i8 quickcraft_type = -1;
    };

    Sync& sync() { return kind_ == Kind::kInventory ? own_ : open_; }
    const Sync& sync() const { return kind_ == Kind::kInventory ? own_ : open_; }
    SlotKind kind_of(i32 slot) const;
    i32 inventory_base() const;  // the first slot of the 27 + 9 inventory slots every menu ends with (own: 9)

    // Slot behaviour.
    u32 slot_limit(i32 slot) const;                                                          // Slot.getMaxStackSize()
    u32 max_for(i32 slot, const ItemStack& s) const;                                         // Slot.getMaxStackSize(stack)
    bool may_place(i32 slot, const ItemStack& s) const;                                      // Slot.mayPlace
    bool may_pickup(i32 slot, const Ctx& cx) const;                                          // Slot.mayPickup
    void set(i32 slot, ItemStack s, const Ctx& cx);                                          // Slot.set: Container.setItem
    void set_by_player(i32 slot, ItemStack s, const Ctx& cx);                                // Slot.setByPlayer(stack)
    ItemStack remove(i32 slot, u32 amount, const Ctx& cx);                                   // Slot.remove: Container.removeItem
    bool try_remove(i32 slot, u32 amount, u32 max_amount, const Ctx& cx, ItemStack& taken);  // Slot.tryRemove
    ItemStack safe_take(i32 slot, u32 amount, u32 max_amount, const Ctx& cx);                // Slot.safeTake
    ItemStack safe_insert(i32 slot, ItemStack input, u32 amount, const Ctx& cx);             // Slot.safeInsert
    void on_take(i32 slot, const Ctx& cx);                                                   // Slot.onTake
    bool quick_replace(i32 slot, const ItemStack& s) const;  // canItemQuickReplace(slot, stack, true)
    void equip_hook(u32 equip_slot, const ItemStack& old, const ItemStack& now, const Ctx& cx);  // LivingEntity.onEquipItem
    void grid_changed(const Ctx& cx);  // slotsChanged(craftSlots): CraftingMenu.slotChangedCraftingGrid

    bool move_to(ItemStack& stack, i32 first, i32 end, bool backwards, const Ctx& cx);  // moveItemStackTo
    ItemStack quick_move(i32 slot, const Ctx& cx);                                      // quickMoveStack
    void reset_quickcraft() {
        sync().quickcraft_status = 0;
        sync().quickcraft_slots[0] = sync().quickcraft_slots[1] = 0;
    }
    u32 next_state_id() { return sync().state_id = (sync().state_id + 1) & 32767; }  // incrementStateId

    Kind kind_ = Kind::kInventory;
    u8 container_id_ = 0;
    u8 rows_ = 0;             // ChestMenu.containerRows
    Containers c_;            // of the operation in progress, or of the last one
    ItemStack result_;        // slot 0 of the player's own menu
    ItemStack table_[9];      // the 3x3 grid of an open CraftingMenu, row by row
    ItemStack table_result_;  // ...and its result slot
    ItemStack own_carried_;   // the cursor of the player's own menu while another one is open
    Sync own_;                // of the player's own menu
    Sync open_;               // of the chest, furnace or crafting menu that is open
};

}  // namespace server
