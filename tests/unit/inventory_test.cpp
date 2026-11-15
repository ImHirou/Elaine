// The inventory and its menu, click by click. Expected outcomes are worked out by hand from
// vanilla's AbstractContainerMenu.doClick / InventoryMenu.quickMoveStack / Inventory.add (26.3).
// The same kinds of click, end to end, are what tools/vanilla/inventory_probe.py replays against a
// vanilla server and this one to compare the packets that come back.
#include <gtest/gtest.h>

#include <vector>

#include "proto/frame.hpp"
#include "proto/packets.hpp"
#include "server/inventory.hpp"

#include PROTO_GEN_ITEMS_HPP

using namespace proto;
using item::ItemStack;
using server::Effect;
using server::Effects;
using server::Inventory;
using server::Menu;
namespace it = proto::gen::item;
namespace ms = server::menu_slot;
namespace inv = server::inv;
using C = sb::ContainerClick;

namespace {

ItemStack stone(u32 n) { return ItemStack(it::kStone, n); }
ItemStack dirt(u32 n) { return ItemStack(it::kDirt, n); }

// What a slot holds, for messages: "stone x 3" reads better than two numbers.
struct Is {
    u32 item, count;
    bool operator==(const Is&) const = default;
};
Is is(const ItemStack& s) { return {s.item(), s.count()}; }
Is is(u32 item_id, u32 count) { return {item_id, count}; }
const Is kNothing{0, 0};
void PrintTo(const Is& s, std::ostream* os) { *os << "item " << s.item << " x " << s.count; }

struct Fixture {
    Menu m;
    Effects fx;
    bool creative = false;
    bool click(i32 slot, i32 button, u32 input) { return m.click(slot, button, input, {creative, fx}); }
    bool left(i32 slot) { return click(slot, 0, C::kPickup); }
    bool right(i32 slot) { return click(slot, 1, C::kPickup); }
    bool shift(i32 slot) { return click(slot, 0, C::kQuickMove); }
    // A drag: QUICK_CRAFT start, one click per slot, end. type 0 left, 1 right, 2 middle.
    void drag(int type, std::initializer_list<i32> slots) {
        EXPECT_TRUE(click(ms::kOutside, 0 | (type << 2), C::kQuickCraft));
        for (const i32 slot : slots) EXPECT_TRUE(click(slot, 1 | (type << 2), C::kQuickCraft));
        EXPECT_TRUE(click(ms::kOutside, 2 | (type << 2), C::kQuickCraft));
    }
    Is at(i32 slot) { return is(m.at(slot)); }
    Is cursor() { return is(m.carried); }
    u32 total(u32 item_id) {  // everything of one item, anywhere in the menu
        u32 n = m.carried.item() == item_id ? m.carried.count() : 0;
        for (i32 s = 0; s < ms::kCount; ++s) n += m.at(s).item() == item_id ? m.at(s).count() : 0;
        for (const Effect& e : fx) n += e.kind == Effect::kDrop && e.stack.item() == item_id ? e.stack.count() : 0;
        return n;
    }
};

}  // namespace

// ---------------------------------------------------------------------------- slots

TEST(Inventory, MenuSlotsMapOntoInventoryIndices) {
    Menu m;
    for (u32 i = 0; i < inv::kSize; ++i) m.inv.items[i] = ItemStack(it::kStone, i + 1);
    EXPECT_EQ(m.at(36).count(), 1u);  // hotbar 0
    EXPECT_EQ(m.at(44).count(), 9u);  // hotbar 8
    EXPECT_EQ(m.at(9).count(), 10u);  // the first slot above the hotbar
    EXPECT_EQ(m.at(35).count(), 36u);
    EXPECT_EQ(m.at(5).count(), inv::kHead + 1);  // head, chest, legs, feet = 39, 38, 37, 36
    EXPECT_EQ(m.at(6).count(), inv::kChest + 1);
    EXPECT_EQ(m.at(7).count(), inv::kLegs + 1);
    EXPECT_EQ(m.at(8).count(), inv::kFeet + 1);
    EXPECT_EQ(m.at(45).count(), inv::kOffhand + 1);
    EXPECT_TRUE(m.at(0).empty());
    for (u32 i = 0; i <= inv::kOffhand; ++i) EXPECT_EQ(&m.at(Menu::slot_of(i)), &m.inv.items[i]) << i;
    EXPECT_EQ(Menu::slot_of(inv::kBody), -1);
    EXPECT_EQ(Menu::slot_of(inv::kSaddle), -1);
    m.craft[2] = dirt(7);
    EXPECT_EQ(m.at(3).item(), it::kDirt);
}

// ---------------------------------------------------------------------------- Inventory.add

TEST(Inventory, AddFillsStacksThenFreeSlots) {
    Inventory in;
    ItemStack s = stone(10);
    EXPECT_TRUE(in.add(-1, s, false));
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(is(in.items[0]), is(it::kStone, 10));  // the first free slot: the hotbar

    // A partial stack is topped up before a new slot is opened: the hand first, then the off
    // hand, then in slot order.
    in.items[20] = stone(60);
    in.items[inv::kOffhand] = stone(63);
    in.selected = 3;
    in.items[3] = stone(62);
    s = stone(64);
    EXPECT_TRUE(in.add(-1, s, false));
    EXPECT_EQ(in.items[3].count(), 64u);              // +2
    EXPECT_EQ(in.items[inv::kOffhand].count(), 64u);  // +1
    EXPECT_EQ(in.items[0].count(), 64u);              // +54
    EXPECT_EQ(in.items[20].count(), 64u);             // +4
    EXPECT_EQ(is(in.items[1]), is(it::kStone, 3));    // the rest
    EXPECT_EQ(in.free_slot(), 2);
}

TEST(Inventory, AddIntoAFullInventory) {
    Inventory in;
    for (u32 i = 0; i < inv::kItems; ++i) in.items[i] = dirt(64);
    ItemStack s = stone(5);
    EXPECT_FALSE(in.add(-1, s, false));
    EXPECT_EQ(s.count(), 5u);
    EXPECT_TRUE(in.add(-1, s, true));  // creative: what does not fit is destroyed
    EXPECT_TRUE(s.empty());

    // Room for 4 of 10: they go in, and vanilla still calls it a failure outside creative.
    in.items[7] = stone(60);
    s = stone(10);
    EXPECT_FALSE(in.add(-1, s, false));
    EXPECT_EQ(s.count(), 6u);
    EXPECT_EQ(in.items[7].count(), 64u);
    in.items[7] = stone(60);
    s = stone(10);
    EXPECT_TRUE(in.add(-1, s, true));
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(in.items[7].count(), 64u);
}

TEST(Inventory, UnstackablesTakeASlotEach) {
    Inventory in;
    in.items[0] = ItemStack(it::kDiamondSword, 1);
    ItemStack s(it::kDiamondSword, 1);
    EXPECT_TRUE(in.add(-1, s, false));
    EXPECT_EQ(is(in.items[1]), is(it::kDiamondSword, 1));
    EXPECT_EQ(in.slot_with_remaining_space(ItemStack(it::kDiamondSword, 1)), -1);
    EXPECT_EQ(in.find_matching(ItemStack(it::kDiamondSword, 1)), 0);
    EXPECT_EQ(in.find_matching(stone(1)), -1);
}

TEST(Inventory, PlaceBackReportsSlotsAndDropsTheRest) {
    Inventory in;
    Effects fx;
    in.items[4] = stone(60);
    in.place_back(stone(10), false, fx);
    ASSERT_EQ(fx.size(), 2u);  // one set_player_inventory per slot touched, each with what it held then
    EXPECT_EQ(fx[0].kind, Effect::kInventorySlot);
    EXPECT_EQ(fx[0].slot, 4);
    EXPECT_EQ(fx[0].stack.count(), 64u);
    EXPECT_EQ(fx[1].slot, 0);
    EXPECT_EQ(fx[1].stack.count(), 6u);

    fx.clear();
    for (u32 i = 0; i < inv::kItems; ++i) in.items[i] = dirt(64);
    in.place_back(stone(10), false, fx);
    ASSERT_EQ(fx.size(), 1u);  // nowhere to go: dropped, not thrown, and the client did not see it coming
    EXPECT_EQ(fx[0].kind, Effect::kDrop);
    EXPECT_FALSE(fx[0].thrown);
    EXPECT_FALSE(fx[0].predicted);
    EXPECT_EQ(is(fx[0].stack), is(it::kStone, 10));
}

// ---------------------------------------------------------------------------- plain clicks

TEST(MenuClick, PickUpAndPutDown) {
    Fixture f;
    f.m.at(10) = stone(5);
    EXPECT_TRUE(f.left(10));  // all of it
    EXPECT_EQ(f.cursor(), is(it::kStone, 5));
    EXPECT_EQ(f.at(10), kNothing);
    EXPECT_TRUE(f.right(11));  // one down
    EXPECT_EQ(f.at(11), is(it::kStone, 1));
    EXPECT_EQ(f.cursor(), is(it::kStone, 4));
    EXPECT_TRUE(f.right(11));  // one more onto the same
    EXPECT_EQ(f.at(11), is(it::kStone, 2));
    EXPECT_TRUE(f.left(12));  // the rest
    EXPECT_EQ(f.at(12), is(it::kStone, 3));
    EXPECT_EQ(f.cursor(), kNothing);
    EXPECT_TRUE(f.right(12));  // the larger half of three
    EXPECT_EQ(f.cursor(), is(it::kStone, 2));
    EXPECT_EQ(f.at(12), is(it::kStone, 1));
    EXPECT_TRUE(f.fx.empty());
    EXPECT_EQ(f.total(it::kStone), 5u);
}

TEST(MenuClick, MergeUpToAStackAndSwapDifferentItems) {
    Fixture f;
    f.m.at(10) = stone(10);
    f.m.carried = stone(60);
    EXPECT_TRUE(f.left(10));
    EXPECT_EQ(f.at(10), is(it::kStone, 64));
    EXPECT_EQ(f.cursor(), is(it::kStone, 6));
    EXPECT_TRUE(f.left(10));  // full: nothing moves
    EXPECT_EQ(f.cursor(), is(it::kStone, 6));
    f.m.at(11) = dirt(3);
    EXPECT_TRUE(f.left(11));  // something else: trade places
    EXPECT_EQ(f.at(11), is(it::kStone, 6));
    EXPECT_EQ(f.cursor(), is(it::kDirt, 3));
    EXPECT_TRUE(f.right(11));  // right click swaps as well
    EXPECT_EQ(f.at(11), is(it::kDirt, 3));
    EXPECT_EQ(f.cursor(), is(it::kStone, 6));
}

TEST(MenuClick, OutsideTheWindowDrops) {
    Fixture f;
    f.m.carried = stone(5);
    EXPECT_TRUE(f.right(ms::kOutside));  // one
    ASSERT_EQ(f.fx.size(), 1u);
    EXPECT_EQ(f.fx[0].kind, Effect::kDrop);
    EXPECT_TRUE(f.fx[0].thrown && f.fx[0].predicted);
    EXPECT_EQ(is(f.fx[0].stack), is(it::kStone, 1));
    EXPECT_EQ(f.cursor(), is(it::kStone, 4));
    EXPECT_TRUE(f.left(ms::kOutside));  // the rest
    ASSERT_EQ(f.fx.size(), 2u);
    EXPECT_EQ(is(f.fx[1].stack), is(it::kStone, 4));
    EXPECT_EQ(f.cursor(), kNothing);
    EXPECT_TRUE(f.left(ms::kOutside));  // nothing to drop
    EXPECT_EQ(f.fx.size(), 2u);
    // The slot index -1 (the window's border) and other negatives do nothing.
    f.m.carried = stone(1);
    EXPECT_TRUE(f.left(-1));
    EXPECT_TRUE(f.shift(-1));
    EXPECT_EQ(f.cursor(), is(it::kStone, 1));
}

TEST(MenuClick, ArmourSlotsTakeWhatBelongsThereOneAtATime) {
    Fixture f;
    f.m.carried = stone(5);
    EXPECT_TRUE(f.left(5));  // stone is not a helmet
    EXPECT_EQ(f.at(5), kNothing);
    EXPECT_EQ(f.cursor(), is(it::kStone, 5));
    f.m.carried = ItemStack(it::kDiamondHelmet, 1);
    EXPECT_TRUE(f.left(6));  // nor is a helmet a chestplate
    EXPECT_EQ(f.at(6), kNothing);
    EXPECT_TRUE(f.left(5));
    EXPECT_EQ(f.at(5), is(it::kDiamondHelmet, 1));
    EXPECT_EQ(f.cursor(), kNothing);
    ASSERT_EQ(f.fx.size(), 1u);  // LivingEntity.onEquipItem: the sound of it going on
    EXPECT_EQ(f.fx[0].kind, Effect::kEquipSound);
    EXPECT_EQ(f.fx[0].value, world::item_info(it::kDiamondHelmet).equip_sound);
    EXPECT_NE(f.fx[0].value, 0);
    f.fx.clear();

    // A stack of pumpkins: one goes on the head. With one on already, the stack cannot trade places.
    f.m.at(5) = {};
    f.m.carried = ItemStack(it::kCarvedPumpkin, 5);
    EXPECT_TRUE(f.left(5));
    EXPECT_EQ(f.at(5), is(it::kCarvedPumpkin, 1));
    EXPECT_EQ(f.cursor(), is(it::kCarvedPumpkin, 4));
    EXPECT_EQ(f.fx.size(), 1u);
    f.m.at(5) = ItemStack(it::kDiamondHelmet, 1);
    EXPECT_TRUE(f.left(5));
    EXPECT_EQ(f.at(5), is(it::kDiamondHelmet, 1));
    EXPECT_EQ(f.cursor(), is(it::kCarvedPumpkin, 4));
    // One pumpkin can.
    f.fx.clear();
    f.m.carried = ItemStack(it::kCarvedPumpkin, 1);
    EXPECT_TRUE(f.left(5));
    EXPECT_EQ(f.at(5), is(it::kCarvedPumpkin, 1));
    EXPECT_EQ(f.cursor(), is(it::kDiamondHelmet, 1));
    EXPECT_EQ(f.fx.size(), 1u);
    // Taking it off makes no sound.
    f.fx.clear();
    f.m.carried = {};
    EXPECT_TRUE(f.left(5));
    EXPECT_EQ(f.cursor(), is(it::kCarvedPumpkin, 1));
    EXPECT_TRUE(f.fx.empty());
    // The shield slot takes anything; a shield going in sounds.
    f.m.carried = stone(3);
    EXPECT_TRUE(f.left(ms::kOffhand));
    EXPECT_EQ(f.at(ms::kOffhand), is(it::kStone, 3));
    EXPECT_TRUE(f.fx.empty());
    f.m.carried = ItemStack(it::kShield, 1);
    EXPECT_TRUE(f.left(ms::kOffhand));
    EXPECT_EQ(f.at(ms::kOffhand), is(it::kShield, 1));
    EXPECT_EQ(f.fx.size(), 1u);
}

TEST(MenuClick, TheResultSlotTakesNothing) {
    Fixture f;
    f.m.carried = stone(5);
    EXPECT_TRUE(f.left(ms::kResult));
    EXPECT_TRUE(f.right(ms::kResult));
    EXPECT_EQ(f.at(ms::kResult), kNothing);
    EXPECT_EQ(f.cursor(), is(it::kStone, 5));
    EXPECT_TRUE(f.left(2));  // the grid is plain storage
    EXPECT_EQ(f.at(2), is(it::kStone, 5));
    EXPECT_EQ(is(f.m.craft[1]), is(it::kStone, 5));
}

// ---------------------------------------------------------------------------- shift-click

TEST(MenuClick, QuickMoveBetweenInventoryAndHotbar) {
    Fixture f;
    f.m.at(10) = stone(20);
    f.m.at(38) = stone(60);  // a partial stack in the hotbar is filled first
    EXPECT_TRUE(f.shift(10));
    EXPECT_EQ(f.at(38), is(it::kStone, 64));
    EXPECT_EQ(f.at(36), is(it::kStone, 16));  // then the first empty hotbar slot
    EXPECT_EQ(f.at(10), kNothing);
    EXPECT_TRUE(f.shift(36));  // and back: hotbar -> the rows above
    EXPECT_EQ(f.at(9), is(it::kStone, 16));
    EXPECT_EQ(f.at(36), kNothing);
    EXPECT_TRUE(f.shift(20));  // an empty slot: nothing
    EXPECT_EQ(f.total(it::kStone), 80u);

    // Nowhere to go: the stack stays, and the click ends (no endless retry).
    for (i32 s = ms::kHotbarFirst; s < ms::kHotbarEnd; ++s) f.m.at(s) = dirt(64);
    EXPECT_TRUE(f.shift(9));
    EXPECT_EQ(f.at(9), is(it::kStone, 16));
}

TEST(MenuClick, QuickMoveWearsArmourAndShields) {
    Fixture f;
    f.m.at(10) = ItemStack(it::kIronChestplate, 1);
    f.m.at(11) = ItemStack(it::kLeatherBoots, 1);
    f.m.at(12) = ItemStack(it::kShield, 1);
    f.m.at(13) = ItemStack(it::kIronChestplate, 1);
    EXPECT_TRUE(f.shift(10));
    EXPECT_EQ(f.at(6), is(it::kIronChestplate, 1));
    EXPECT_TRUE(f.shift(11));
    EXPECT_EQ(f.at(8), is(it::kLeatherBoots, 1));
    EXPECT_TRUE(f.shift(12));
    EXPECT_EQ(f.at(ms::kOffhand), is(it::kShield, 1));
    EXPECT_EQ(f.fx.size(), 3u);  // three things went on
    for (const Effect& e : f.fx) EXPECT_EQ(e.kind, Effect::kEquipSound);
    EXPECT_TRUE(f.shift(13));  // a chestplate is on already: this one goes to the hotbar
    EXPECT_EQ(f.at(36), is(it::kIronChestplate, 1));
    EXPECT_EQ(f.fx.size(), 3u);
    EXPECT_TRUE(f.shift(6));  // off the body: the first free slot from 9 up
    EXPECT_EQ(f.at(6), kNothing);
    EXPECT_EQ(f.at(9), is(it::kIronChestplate, 1));
    EXPECT_EQ(f.fx.size(), 3u);
    // Horse armour has an equipment slot too, but none a player has: it moves like a rock.
    f.m.at(20) = ItemStack(it::kDiamondHorseArmor, 1);
    EXPECT_TRUE(f.shift(20));
    EXPECT_EQ(f.at(37), is(it::kDiamondHorseArmor, 1));
}

// ---------------------------------------------------------------------------- number keys, F

TEST(MenuClick, SwapWithHotbarKey) {
    Fixture f;
    f.m.at(10) = stone(7);
    EXPECT_TRUE(f.click(10, 2, C::kSwap));  // key 3: into an empty hotbar slot
    EXPECT_EQ(f.at(38), is(it::kStone, 7));
    EXPECT_EQ(f.at(10), kNothing);
    f.m.at(11) = dirt(2);
    EXPECT_TRUE(f.click(11, 2, C::kSwap));  // both hold something: they trade
    EXPECT_EQ(f.at(38), is(it::kDirt, 2));
    EXPECT_EQ(f.at(11), is(it::kStone, 7));
    EXPECT_TRUE(f.click(12, 2, C::kSwap));  // into an empty slot
    EXPECT_EQ(f.at(12), is(it::kDirt, 2));
    EXPECT_EQ(f.at(38), kNothing);
    EXPECT_TRUE(f.click(13, 2, C::kSwap));   // nothing on either side
    EXPECT_TRUE(f.click(12, 40, C::kSwap));  // F: the off hand
    EXPECT_EQ(f.at(ms::kOffhand), is(it::kDirt, 2));
    EXPECT_TRUE(f.click(12, 9, C::kSwap));  // not a key
    EXPECT_TRUE(f.click(12, -1, C::kSwap));
    EXPECT_EQ(f.at(ms::kOffhand), is(it::kDirt, 2));
    EXPECT_TRUE(f.click(ms::kOffhand, 40, C::kSwap));  // the slot's own key
    EXPECT_EQ(f.at(ms::kOffhand), is(it::kDirt, 2));

    // Armour slots: what does not belong is not swapped in; a stack of pumpkins leaves one.
    f.m.inv.items[0] = stone(3);
    EXPECT_TRUE(f.click(5, 0, C::kSwap));
    EXPECT_EQ(f.at(5), kNothing);
    f.m.inv.items[0] = ItemStack(it::kCarvedPumpkin, 4);
    EXPECT_TRUE(f.click(5, 0, C::kSwap));
    EXPECT_EQ(f.at(5), is(it::kCarvedPumpkin, 1));
    EXPECT_EQ(is(f.m.inv.items[0]), is(it::kCarvedPumpkin, 3));
    // With a helmet on, the stack swaps one in and the helmet looks for room elsewhere.
    f.m.at(5) = ItemStack(it::kDiamondHelmet, 1);
    EXPECT_TRUE(f.click(5, 0, C::kSwap));
    EXPECT_EQ(f.at(5), is(it::kCarvedPumpkin, 1));
    EXPECT_EQ(is(f.m.inv.items[0]), is(it::kCarvedPumpkin, 2));
    EXPECT_EQ(is(f.m.inv.items[1]), is(it::kDiamondHelmet, 1));

    EXPECT_FALSE(f.click(-1, 0, C::kSwap));  // vanilla indexes its slot list with this
    EXPECT_FALSE(f.click(ms::kOutside, 0, C::kSwap));
}

// ---------------------------------------------------------------------------- middle click, Q, double click

TEST(MenuClick, CloneIsForCreative) {
    Fixture f;
    f.m.at(10) = stone(3);
    EXPECT_TRUE(f.click(10, 2, C::kClone));
    EXPECT_EQ(f.cursor(), kNothing);
    f.creative = true;
    EXPECT_TRUE(f.click(10, 2, C::kClone));
    EXPECT_EQ(f.cursor(), is(it::kStone, 64));  // a full stack, whatever the slot holds
    EXPECT_EQ(f.at(10), is(it::kStone, 3));
    EXPECT_TRUE(f.click(11, 2, C::kClone));  // not with something on the cursor
    EXPECT_EQ(f.cursor(), is(it::kStone, 64));
}

TEST(MenuClick, ThrowFromASlot) {
    Fixture f;
    f.m.at(10) = stone(5);
    EXPECT_TRUE(f.click(10, 0, C::kThrow));  // Q: one
    ASSERT_EQ(f.fx.size(), 1u);
    EXPECT_EQ(is(f.fx[0].stack), is(it::kStone, 1));
    EXPECT_TRUE(f.fx[0].thrown && f.fx[0].predicted);
    EXPECT_EQ(f.at(10), is(it::kStone, 4));
    EXPECT_TRUE(f.click(10, 1, C::kThrow));  // ctrl+Q: the stack
    ASSERT_EQ(f.fx.size(), 2u);
    EXPECT_EQ(is(f.fx[1].stack), is(it::kStone, 4));
    EXPECT_EQ(f.at(10), kNothing);
    EXPECT_TRUE(f.click(10, 1, C::kThrow));  // an empty slot throws nothing
    EXPECT_EQ(f.fx.size(), 2u);
    f.m.at(10) = stone(1);
    f.m.carried = dirt(1);
    EXPECT_TRUE(f.click(10, 0, C::kThrow));  // not with something on the cursor
    EXPECT_EQ(f.fx.size(), 2u);
}

TEST(MenuClick, DoubleClickGathers) {
    Fixture f;
    f.m.carried = stone(10);
    f.m.at(9) = stone(64);
    f.m.at(10) = stone(5);
    f.m.at(20) = stone(20);
    f.m.at(21) = dirt(9);
    f.m.at(2) = stone(3);  // the crafting grid counts
    EXPECT_TRUE(f.click(15, 0, C::kPickupAll));
    // Partial stacks first, in slot order (3 + 5 + 20), then from the full one.
    EXPECT_EQ(f.cursor(), is(it::kStone, 64));
    EXPECT_EQ(f.at(2), kNothing);
    EXPECT_EQ(f.at(10), kNothing);
    EXPECT_EQ(f.at(20), kNothing);
    EXPECT_EQ(f.at(9), is(it::kStone, 38));
    EXPECT_EQ(f.at(21), is(it::kDirt, 9));
    EXPECT_EQ(f.total(it::kStone), 102u);
    // On a slot that holds something takeable, it is not a gather.
    f.m.carried = stone(1);
    EXPECT_TRUE(f.click(9, 0, C::kPickupAll));
    EXPECT_EQ(f.cursor(), is(it::kStone, 1));
}

// ---------------------------------------------------------------------------- drags

TEST(MenuClick, DragSpreadsEvenlyOrOneEach) {
    Fixture f;
    f.m.carried = stone(10);
    f.drag(0, {10, 11, 12});  // left: floor(10 / 3) each, the rest stays
    EXPECT_EQ(f.at(10), is(it::kStone, 3));
    EXPECT_EQ(f.at(11), is(it::kStone, 3));
    EXPECT_EQ(f.at(12), is(it::kStone, 3));
    EXPECT_EQ(f.cursor(), is(it::kStone, 1));

    f.m.carried = stone(10);
    f.drag(1, {10, 13});  // right: one each, onto what is there
    EXPECT_EQ(f.at(10), is(it::kStone, 4));
    EXPECT_EQ(f.at(13), is(it::kStone, 1));
    EXPECT_EQ(f.cursor(), is(it::kStone, 8));

    f.m.at(14) = stone(63);
    f.m.at(15) = dirt(1);
    f.drag(0, {14, 15, 16});  // 15 holds something else and never joins: 8 over two slots, one of them nearly full
    EXPECT_EQ(f.at(14), is(it::kStone, 64));
    EXPECT_EQ(f.at(15), is(it::kDirt, 1));
    EXPECT_EQ(f.at(16), is(it::kStone, 4));
    EXPECT_EQ(f.cursor(), is(it::kStone, 3));
}

TEST(MenuClick, DragOverOneSlotIsAClickAndAStrayClickCancels) {
    Fixture f;
    f.m.carried = stone(10);
    f.drag(1, {10});  // a right-drag that never left its slot
    EXPECT_EQ(f.at(10), is(it::kStone, 1));
    EXPECT_EQ(f.cursor(), is(it::kStone, 9));
    f.drag(0, {11});
    EXPECT_EQ(f.at(11), is(it::kStone, 9));
    EXPECT_EQ(f.cursor(), kNothing);

    f.m.carried = stone(10);
    EXPECT_TRUE(f.click(ms::kOutside, 0, C::kQuickCraft));  // start
    EXPECT_TRUE(f.click(12, 1, C::kQuickCraft));
    EXPECT_TRUE(f.left(13));  // an ordinary click in the middle: it only ends the drag
    EXPECT_EQ(f.at(13), kNothing);
    EXPECT_TRUE(f.click(ms::kOutside, 2, C::kQuickCraft));  // the end has nothing to end
    EXPECT_EQ(f.at(12), kNothing);
    EXPECT_EQ(f.cursor(), is(it::kStone, 10));
    // An "add" out of the blue, and one with a slot vanilla would trip over.
    EXPECT_TRUE(f.click(12, 1, C::kQuickCraft));
    EXPECT_EQ(f.at(12), kNothing);
    EXPECT_TRUE(f.click(ms::kOutside, 0, C::kQuickCraft));
    EXPECT_FALSE(f.click(-1, 1, C::kQuickCraft));
}

TEST(MenuClick, MiddleDragIsCreativeOnlyAndGivesFullStacks) {
    Fixture f;
    f.m.carried = stone(3);
    f.drag(2, {10, 11});
    EXPECT_EQ(f.at(10), kNothing);
    f.creative = true;
    f.drag(2, {10, 11});
    EXPECT_EQ(f.at(10), is(it::kStone, 64));
    EXPECT_EQ(f.at(11), is(it::kStone, 64));
    EXPECT_EQ(f.cursor(), kNothing);  // "remaining" went far below zero
}

// ---------------------------------------------------------------------------- other packets

TEST(Menu, CreativeSlotAndClose) {
    Fixture f;
    f.creative = true;
    const Menu::Ctx cx{true, f.fx};
    f.m.set_creative(36, stone(64), cx);
    f.m.set_creative(5, stone(1), cx);  // the creative inventory may put anything anywhere
    EXPECT_EQ(f.at(5), is(it::kStone, 1));
    EXPECT_TRUE(f.fx.empty());
    f.m.set_creative(6, ItemStack(it::kElytra, 1), cx);
    EXPECT_EQ(f.fx.size(), 1u);
    f.fx.clear();
    f.m.set_creative(1, dirt(4), cx);
    f.m.set_creative(4, stone(10), cx);
    ASSERT_EQ(f.fx.size(), 2u);  // the crafting grid changed twice: the result slot is sent each time
    EXPECT_EQ(f.fx[0].kind, Effect::kSetSlot);
    EXPECT_TRUE(f.fx[1].stack.empty());  // dirt and stone make nothing
    f.fx.clear();
    f.m.carried = dirt(2);
    f.m.close(cx);  // the cursor and the grid return to the inventory
    EXPECT_EQ(f.cursor(), kNothing);
    EXPECT_EQ(f.at(1), kNothing);
    EXPECT_EQ(f.at(4), kNothing);
    EXPECT_EQ(f.at(37), is(it::kDirt, 6));    // the cursor's 2, then the grid's 4 on top
    EXPECT_EQ(f.at(38), is(it::kStone, 10));  // 36 is full
    ASSERT_EQ(f.fx.size(), 3u);
    EXPECT_EQ(f.fx[0].kind, Effect::kInventorySlot);
    EXPECT_EQ(f.fx[0].slot, 1);  // inventory index, not menu slot
    EXPECT_EQ(is(f.fx[0].stack), is(it::kDirt, 2));
    EXPECT_EQ(is(f.fx[1].stack), is(it::kDirt, 6));
    EXPECT_EQ(f.fx[2].slot, 2);
}

TEST(Menu, HandsAndPicking) {
    Fixture f;
    const Menu::Ctx cx{true, f.fx};
    f.m.inv.selected = 2;
    f.m.inv.items[2] = stone(5);
    EXPECT_EQ(is(f.m.take_from_hand(false)), is(it::kStone, 1));
    EXPECT_EQ(is(f.m.take_from_hand(true)), is(it::kStone, 4));
    EXPECT_TRUE(f.m.take_from_hand(true).empty());

    f.m.inv.items[2] = ItemStack(it::kShield, 1);
    f.m.inv.items[inv::kOffhand] = stone(9);
    f.m.swap_hands(cx);
    EXPECT_EQ(is(f.m.inv.items[2]), is(it::kStone, 9));
    EXPECT_EQ(is(f.m.inv.items[inv::kOffhand]), is(it::kShield, 1));
    EXPECT_EQ(f.fx.size(), 1u);  // a shield went onto the arm
    f.fx.clear();
    f.m.swap_hands(cx);
    EXPECT_TRUE(f.fx.empty());  // into the main hand it is just an item

    // tryPickItem: select it, fetch it, or (creative) make it.
    Menu m;
    m.inv.items[4] = stone(1);
    m.inv.items[20] = dirt(8);
    m.pick(stone(1), false);
    EXPECT_EQ(m.inv.selected, 4);
    m.pick(dirt(1), false);  // from the back rows into the first empty hotbar slot counting from the hand
    EXPECT_EQ(m.inv.selected, 5);
    EXPECT_EQ(is(m.inv.items[5]), is(it::kDirt, 8));
    EXPECT_TRUE(m.inv.items[20].empty());
    m.pick(ItemStack(it::kGlowstone, 1), false);  // not there, not creative
    EXPECT_EQ(m.inv.selected, 5);
    m.pick(ItemStack(it::kGlowstone, 1), true);
    EXPECT_EQ(m.inv.selected, 6);
    EXPECT_EQ(is(m.inv.items[6]), is(it::kGlowstone, 1));
    // A full hotbar: the hand's stack moves out to a free slot and the new one takes its place.
    for (u32 i = 0; i < inv::kHotbarSize; ++i) m.inv.items[i] = stone(i + 1);
    m.inv.selected = 3;
    m.pick(ItemStack(it::kGlowstone, 1), true);
    EXPECT_EQ(m.inv.selected, 3);
    EXPECT_EQ(is(m.inv.items[3]), is(it::kGlowstone, 1));
    EXPECT_EQ(is(m.inv.items[9]), is(it::kStone, 4));
}

// ---------------------------------------------------------------------------- synchronisation

namespace {

struct Sent {
    u32 id;
    std::vector<u8> body;
};
std::vector<Sent> packets(base::ByteBuf& buf) {
    std::vector<Sent> out;
    const u8* p = buf.data;
    const u8* end = buf.data + buf.len;
    while (p < end) {
        std::span<const u8> frame;
        u32 used = 0;
        EXPECT_EQ(frame_split(p, end, kMaxFrame, frame, used), FrameStatus::Ok);
        Reader r(frame);
        const u32 id = r.varint();
        out.push_back({id, {r.p, r.end}});
        p += used;
    }
    buf.clear();
    return out;
}

}  // namespace

TEST(MenuSync, OnlyWhatTheClientDoesNotKnow) {
    namespace cbid = ids::play::cb;
    Menu m;
    base::ByteBuf out;
    m.inv.items[0] = stone(3);
    m.send_all(out, kNoCompression);
    auto sent = packets(out);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].id, cbid::kContainerSetContent);
    {
        Reader r(sent[0].body.data(), sent[0].body.size());
        EXPECT_EQ(r.varint(), 0u);  // container id
        EXPECT_EQ(r.varint(), 1u);  // the first state id
        EXPECT_EQ(r.varint(), 46u);
        for (int slot = 0; slot < 46; ++slot) {
            const u32 count = r.varint();
            EXPECT_EQ(count, slot == 36 ? 3u : 0u) << slot;
            if (count) {
                EXPECT_EQ(r.varint(), it::kStone);
                EXPECT_EQ(r.be16(), 0);  // no components
            }
        }
        EXPECT_EQ(r.varint(), 0u);  // nothing on the cursor
        EXPECT_TRUE(r.done());
    }
    EXPECT_EQ(m.state_id(), 1u);

    m.send_changes(out, kNoCompression);  // the client knows everything
    EXPECT_EQ(out.len, 0u);

    m.inv.items[0].grow(1);
    m.at(10) = dirt(2);
    m.carried = stone(1);
    m.send_changes(out, kNoCompression);
    sent = packets(out);
    ASSERT_EQ(sent.size(), 3u);  // slots in order, then the cursor
    EXPECT_EQ(sent[0].id, cbid::kContainerSetSlot);
    {
        Reader r(sent[0].body.data(), sent[0].body.size());
        EXPECT_EQ(r.varint(), 0u);
        EXPECT_EQ(r.varint(), 2u);  // every slot packet takes the next state id
        EXPECT_EQ(r.be16(), 10);
        EXPECT_EQ(r.varint(), 2u);
        EXPECT_EQ(r.varint(), it::kDirt);
        EXPECT_EQ(r.be16(), 0);
        EXPECT_TRUE(r.done());
    }
    {
        Reader r(sent[1].body.data(), sent[1].body.size());
        r.varint();
        EXPECT_EQ(r.varint(), 3u);
        EXPECT_EQ(r.be16(), 36);
        EXPECT_EQ(r.varint(), 4u);
    }
    EXPECT_EQ(sent[2].id, cbid::kSetCursorItem);  // no state id on this one
    EXPECT_EQ(sent[2].body, (std::vector<u8>{1, static_cast<u8>(it::kStone), 0, 0}));
    EXPECT_EQ(m.state_id(), 3u);
    m.send_changes(out, kNoCompression);
    EXPECT_EQ(out.len, 0u);

    // What the client did itself (and said so) is not sent back; what it may have wrong is.
    m.at(11) = stone(7);
    m.remote_is(11, stone(7));
    m.send_changes(out, kNoCompression);
    EXPECT_EQ(out.len, 0u);
    m.remote_is(11, stone(6));
    m.remote_is_unknown(12);  // empty here, but who knows what the client shows
    m.remote_carried_is_unknown();
    m.send_changes(out, kNoCompression);
    sent = packets(out);
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[0].id, cbid::kContainerSetSlot);
    EXPECT_EQ(sent[1].id, cbid::kContainerSetSlot);
    EXPECT_EQ(sent[2].id, cbid::kSetCursorItem);
    // The state id wraps at 15 bits.
    for (int i = 0; i < 32767; ++i) {
        m.remote_is_unknown(20);
        m.send_changes(out, kNoCompression);
        out.clear();
    }
    EXPECT_EQ(m.state_id(), 5u + 32767u - 32768u);
}
