// SPDX-License-Identifier: GPL-3.0-or-later
// Recipes, the furnace, and the menus over a chest, a furnace and a crafting grid.
//
// Expected outcomes are worked out by hand from vanilla (26.3): RecipeManager / ShapedRecipePattern
// / ShapelessRecipe / TransmuteRecipe, AbstractFurnaceBlockEntity.serverTick, ChestMenu,
// AbstractFurnaceMenu, CraftingMenu, ResultSlot. The recipes themselves are the jar's (mcgen).
// The same flows, end to end, are what tools/vanilla/container_probe.py replays against a
// vanilla server and this one.
#include <gtest/gtest.h>

#include <vector>

#include "item/recipes.hpp"
#include "proto/frame.hpp"
#include "proto/packets.hpp"
#include "server/block_entity.hpp"
#include "server/inventory.hpp"

#include PROTO_GEN_ITEMS_HPP

using namespace proto;
using item::ItemStack;
using server::Chest;
using server::Containers;
using server::Effect;
using server::Effects;
using server::Furnace;
using server::Menu;
namespace it = proto::gen::item;
namespace inv = server::inv;
using C = sb::ContainerClick;

namespace {

struct Is {
    u32 item, count;
    bool operator==(const Is&) const = default;
};
Is is(const ItemStack& s) { return {s.item(), s.count()}; }
Is is(u32 item_id, u32 count) { return {item_id, count}; }
const Is kNothing{0, 0};
void PrintTo(const Is& s, std::ostream* os) { *os << "item " << s.item << " x " << s.count; }

// A grid from a picture: one letter per cell, '.' for an empty one.
struct Grid {
    ItemStack cell[9];
    Grid(std::string_view rows, std::initializer_list<std::pair<char, u32>> key) {
        u32 k = 0;
        for (const char ch : rows) {
            if (ch == ' ') continue;
            for (const auto& [letter, item_id] : key)
                if (letter == ch) cell[k] = ItemStack(item_id, 1);
            ++k;
        }
    }
};
Is craft3(std::string_view rows, std::initializer_list<std::pair<char, u32>> key) {
    const Grid g(rows, key);
    return is(item::craft(g.cell, 3, 3));
}
Is craft2(std::string_view rows, std::initializer_list<std::pair<char, u32>> key) {
    const Grid g(rows, key);
    return is(item::craft(g.cell, 2, 2));
}

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

// A menu with a chest or a furnace behind it.
struct Fixture {
    Menu m;
    Effects fx;
    Chest chest[2];
    Furnace furnace;
    Containers c;
    Menu::Ctx cx() { return {false, fx, c}; }
    void open_chest(u32 rows) {
        c.chest[0] = chest[0].items;
        c.chest[1] = rows == 6 ? chest[1].items : nullptr;
        m.open(Menu::Kind::kChest, 1, rows);
    }
    void open_furnace() {
        c.furnace = &furnace;
        m.open(Menu::Kind::kFurnace, 2);
    }
    void open_table() { m.open(Menu::Kind::kCrafting, 3); }
    bool click(i32 slot, i32 button, u32 input) { return m.click(slot, button, input, cx()); }
    bool left(i32 slot) { return click(slot, 0, C::kPickup); }
    bool right(i32 slot) { return click(slot, 1, C::kPickup); }
    bool shift(i32 slot) { return click(slot, 0, C::kQuickMove); }
    Is at(i32 slot) {
        m.send_changes(scratch, kNoCompression, c);  // binds the containers for at()
        return is(m.at(slot));
    }
    // The result slot packets (Effect::kSetSlot) since the last call, as what they showed.
    std::vector<Is> results() {
        std::vector<Is> out;
        for (const Effect& e : fx)
            if (e.kind == Effect::kSetSlot) out.push_back(is(e.stack));
        fx.clear();
        return out;
    }
    base::ByteBuf scratch;
};

}  // namespace

// ---------------------------------------------------------------------------- recipes

TEST(Recipes, ShapedAnywhereInTheGridAndMirrored) {
    const auto planks = std::pair{'p', it::kOakPlanks};
    EXPECT_EQ(craft2("pp pp", {planks}), is(it::kCraftingTable, 1));
    EXPECT_EQ(craft3("pp. pp. ...", {planks}), is(it::kCraftingTable, 1));
    EXPECT_EQ(craft3("... .pp .pp", {planks}), is(it::kCraftingTable, 1));  // CraftingInput: the empty border is cut off
    EXPECT_EQ(craft3("..p ..p ...", {planks}), is(it::kStick, 4));
    EXPECT_EQ(craft2("p. p.", {planks}), is(it::kStick, 4));
    EXPECT_EQ(craft3("ppp p.p ppp", {planks}), is(it::kChest, 1));
    // An ingredient is a tag here (#planks): any mix of woods makes a chest.
    EXPECT_EQ(craft3("pbp b.b pbp", {planks, {'b', it::kBirchPlanks}}), is(it::kChest, 1));
    EXPECT_EQ(craft3("ppp ppp ppp", {planks}), kNothing);
    EXPECT_EQ(craft3("p.p ... ...", {planks}), kNothing);  // two planks, but not on top of each other
    // The hoe's pattern is "XX", " #", " #": as laid out, and mirrored - but not upside down.
    const auto iron = std::pair{'i', it::kIronIngot}, stick = std::pair{'s', it::kStick};
    EXPECT_EQ(craft3("ii. .s. .s.", {iron, stick}), is(it::kIronHoe, 1));
    EXPECT_EQ(craft3(".ii .s. .s.", {iron, stick}), is(it::kIronHoe, 1));
    EXPECT_EQ(craft3(".s. .s. ii.", {iron, stick}), kNothing);
    EXPECT_EQ(craft3("... ... ...", {}), kNothing);
}

TEST(Recipes, ShapelessAndTransmute) {
    EXPECT_EQ(craft2("l. ..", {{'l', it::kOakLog}}), is(it::kOakPlanks, 4));
    EXPECT_EQ(craft3("... ..l ...", {{'l', it::kOakLog}}), is(it::kOakPlanks, 4));
    // Every ingredient takes a cell of its own, wherever it lies.
    const auto iron = std::pair{'i', it::kIronIngot}, flint = std::pair{'f', it::kFlint};
    EXPECT_EQ(craft3("f.. ... ..i", {iron, flint}), is(it::kFlintAndSteel, 1));
    EXPECT_EQ(craft2("if ..", {iron, flint}), is(it::kFlintAndSteel, 1));
    EXPECT_EQ(craft2("ii f.", {iron, flint}), kNothing);  // one cell too many
    EXPECT_EQ(craft2("ff ..", {iron, flint}), kNothing);

    // A result with components of its own: the stew remembers its flower.
    const Grid stew("bm rp", {{'b', it::kBowl}, {'m', it::kBrownMushroom}, {'r', it::kRedMushroom}, {'p', it::kPoppy}});
    const ItemStack made = item::craft(stew.cell, 2, 2);
    EXPECT_EQ(is(made), is(it::kSuspiciousStew, 1));
    ASSERT_TRUE(made.has_patch());
    EXPECT_EQ(item::patches().find(made.patch(), item::component::kSuspiciousStewEffects).state, item::Patches::State::kSet);
    EXPECT_TRUE(item::same_item_same_components(made, item::craft(stew.cell, 2, 2)));  // one patch, however often it is made

    // TransmuteRecipe: a bundle and a dye. Dyeing a black bundle black is no recipe.
    EXPECT_EQ(craft2("bd ..", {{'b', it::kBundle}, {'d', it::kBlackDye}}), is(it::kBlackBundle, 1));
    EXPECT_EQ(craft2("d. .b", {{'b', it::kPinkBundle}, {'d', it::kBlackDye}}), is(it::kBlackBundle, 1));
    EXPECT_EQ(craft2("bd ..", {{'b', it::kBlackBundle}, {'d', it::kBlackDye}}), kNothing);
    EXPECT_EQ(craft2("bd d.", {{'b', it::kBundle}, {'d', it::kBlackDye}}), kNothing);  // one dye, no more
}

TEST(Recipes, SmeltingAndFuel) {
    EXPECT_EQ(item::smelting(it::kIronOre).result, it::kIronIngot);
    EXPECT_EQ(item::smelting(it::kIronOre).ticks, 200);
    EXPECT_FLOAT_EQ(item::smelting(it::kIronOre).experience, 0.7f);
    EXPECT_EQ(item::smelting(it::kCobblestone).result, it::kStone);
    EXPECT_EQ(item::smelting(it::kStone).result, it::kSmoothStone);
    EXPECT_EQ(item::smelting(it::kOakLog).count, 1);  // charcoal, from any of #logs_that_burn
    EXPECT_EQ(item::smelting(it::kDirt).result, 0u);
    EXPECT_FALSE(item::smeltable(ItemStack(it::kDirt, 1)));
    EXPECT_TRUE(item::smeltable(ItemStack(it::kCobblestone, 1)));

    EXPECT_EQ(item::fuel_ticks(ItemStack(it::kCoal, 1)), 1600u);
    EXPECT_EQ(item::fuel_ticks(ItemStack(it::kOakPlanks, 3)), 300u);
    EXPECT_EQ(item::fuel_ticks(ItemStack(it::kStick, 1)), 100u);
    EXPECT_EQ(item::fuel_ticks(ItemStack(it::kLavaBucket, 1)), 20000u);
    EXPECT_EQ(item::fuel_ticks(ItemStack(it::kStone, 1)), 0u);
    EXPECT_EQ(item::fuel_ticks({}), 0u);
    EXPECT_EQ(item::crafting_remainder(it::kLavaBucket), it::kBucket);
    EXPECT_EQ(item::crafting_remainder(it::kMilkBucket), it::kBucket);
    EXPECT_EQ(item::crafting_remainder(it::kCoal), 0u);
}

// ---------------------------------------------------------------------------- the furnace

TEST(Furnace, BurnsFuelAndCooks) {
    Furnace f;
    EXPECT_TRUE(f.idle());
    f.set_item(Furnace::kInput, ItemStack(it::kCobblestone, 2));
    EXPECT_EQ(f.cook_total, 200);  // setItem asks the recipe how long it takes
    EXPECT_TRUE(f.idle());         // nothing to burn
    f.set_item(Furnace::kFuel, ItemStack(it::kCoal, 1));
    EXPECT_FALSE(f.idle());

    Furnace::Ticked t = f.tick();  // the coal catches fire, and the first tick of cooking is this one
    EXPECT_TRUE(t.lit_changed);
    EXPECT_TRUE(t.lit);
    EXPECT_TRUE(f.items[Furnace::kFuel].empty());
    EXPECT_EQ(f.lit_remaining, 1600);
    EXPECT_EQ(f.lit_total, 1600);
    EXPECT_EQ(f.cook, 1);
    for (int i = 0; i < 198; ++i) EXPECT_FALSE(f.tick().lit_changed);
    EXPECT_EQ(f.cook, 199);
    EXPECT_TRUE(f.items[Furnace::kResult].empty());
    f.tick();  // the 200th
    EXPECT_EQ(is(f.items[Furnace::kResult]), is(it::kStone, 1));
    EXPECT_EQ(is(f.items[Furnace::kInput]), is(it::kCobblestone, 1));
    EXPECT_EQ(f.cook, 0);
    EXPECT_EQ(f.lit_remaining, 1600 - 199);
    for (int i = 0; i < 200; ++i) f.tick();
    EXPECT_EQ(is(f.items[Furnace::kResult]), is(it::kStone, 2));
    EXPECT_TRUE(f.items[Furnace::kInput].empty());
    EXPECT_EQ(f.data(0), f.lit_remaining);
    EXPECT_EQ(f.data(3), 200);

    // Nothing left to cook: the fire burns down all the same, and only then the block goes dark.
    while (f.lit_remaining > 1) EXPECT_FALSE(f.tick().lit_changed);
    t = f.tick();
    EXPECT_TRUE(t.lit_changed);
    EXPECT_FALSE(t.lit);
    EXPECT_TRUE(f.idle());
}

TEST(Furnace, EdgesOfTheTick) {
    // A lava bucket leaves its bucket in the fuel slot.
    Furnace f;
    f.set_item(Furnace::kInput, ItemStack(it::kIronOre, 1));
    f.set_item(Furnace::kFuel, ItemStack(it::kLavaBucket, 1));
    f.tick();
    EXPECT_EQ(is(f.items[Furnace::kFuel]), is(it::kBucket, 1));
    EXPECT_EQ(f.lit_total, 20000);

    // A result slot that cannot take the result: nothing is lit.
    Furnace full;
    full.set_item(Furnace::kInput, ItemStack(it::kCobblestone, 1));
    full.set_item(Furnace::kFuel, ItemStack(it::kCoal, 1));
    full.set_item(Furnace::kResult, ItemStack(it::kIronIngot, 1));
    EXPECT_FALSE(full.tick().lit_changed);
    EXPECT_EQ(is(full.items[Furnace::kFuel]), is(it::kCoal, 1));
    full.set_item(Furnace::kResult, ItemStack(it::kStone, 64));
    EXPECT_FALSE(full.tick().lit_changed);
    full.set_item(Furnace::kResult, ItemStack(it::kStone, 63));
    EXPECT_TRUE(full.tick().lit_changed);

    // The fuel runs out half-way: what was cooked cools off at twice the speed.
    Furnace cold;
    cold.set_item(Furnace::kInput, ItemStack(it::kCobblestone, 1));
    cold.set_item(Furnace::kFuel, ItemStack(it::kStick, 1));  // 100 ticks
    for (int i = 0; i < 100; ++i) cold.tick();
    EXPECT_EQ(cold.cook, 100);
    EXPECT_EQ(cold.lit_remaining, 1);
    EXPECT_TRUE(cold.tick().lit_changed);  // out: this tick still counted as cooking? no - the fire is gone
    EXPECT_EQ(cold.cook, 98);
    cold.tick();
    EXPECT_EQ(cold.cook, 96);
    // Another kind of input starts from nothing; more of the same does not.
    cold.set_item(Furnace::kInput, ItemStack(it::kCobblestone, 5));
    EXPECT_EQ(cold.cook, 96);
    cold.set_item(Furnace::kInput, ItemStack(it::kIronOre, 1));
    EXPECT_EQ(cold.cook, 0);
    cold.set_item(Furnace::kInput, {});
    EXPECT_EQ(cold.cook_total, 200);  // BURN_TIME_STANDARD for nothing

    // A wet sponge dries into the bucket standing in the fuel slot.
    Furnace sponge;
    sponge.set_item(Furnace::kInput, ItemStack(it::kWetSponge, 1));
    sponge.set_item(Furnace::kFuel, ItemStack(it::kCoal, 1));
    sponge.tick();
    sponge.set_item(Furnace::kFuel, ItemStack(it::kBucket, 1));
    for (int i = 0; i < 199; ++i) sponge.tick();
    EXPECT_EQ(is(sponge.items[Furnace::kResult]), is(it::kSponge, 1));
    EXPECT_EQ(is(sponge.items[Furnace::kFuel]), is(it::kWaterBucket, 1));
}

// ---------------------------------------------------------------------------- menus

TEST(ContainerMenu, ChestSlotsAndShiftClicks) {
    Fixture f;
    f.m.inv.items[0] = ItemStack(it::kStone, 10);  // hotbar 0
    f.m.inv.items[9] = ItemStack(it::kDirt, 5);    // the first slot above the hotbar
    f.chest[0].items[3] = ItemStack(it::kStone, 60);
    f.open_chest(3);
    EXPECT_EQ(f.m.slot_count(), 63);
    EXPECT_EQ(f.m.container_id(), 1u);
    EXPECT_EQ(f.at(3), is(it::kStone, 60));
    EXPECT_EQ(f.at(27), is(it::kDirt, 5));    // the inventory follows the chest
    EXPECT_EQ(f.at(54), is(it::kStone, 10));  // ...and the hotbar the inventory

    EXPECT_TRUE(f.shift(54));  // onto the stack in the chest, the rest into its first free slot
    EXPECT_EQ(is(f.chest[0].items[3]), is(it::kStone, 64));
    EXPECT_EQ(is(f.chest[0].items[0]), is(it::kStone, 6));
    EXPECT_TRUE(f.m.inv.items[0].empty());
    EXPECT_TRUE(f.shift(3));  // out of the chest: the hotbar first, from its far end
    EXPECT_EQ(is(f.m.inv.items[8]), is(it::kStone, 64));
    EXPECT_TRUE(f.chest[0].items[3].empty());

    EXPECT_TRUE(f.left(0));  // pick up, put down half
    EXPECT_EQ(is(f.m.carried), is(it::kStone, 6));
    EXPECT_TRUE(f.right(26));
    EXPECT_EQ(is(f.chest[0].items[26]), is(it::kStone, 1));
    EXPECT_FALSE(f.m.at(26).empty());

    // Closing: the cursor goes back, and the player's own menu knows what the client holds.
    base::ByteBuf out;
    f.m.send_all(out, kNoCompression, f.c);
    out.clear();
    f.m.close(f.cx());
    EXPECT_EQ(f.m.kind(), Menu::Kind::kInventory);
    EXPECT_TRUE(f.m.carried.empty());
    EXPECT_EQ(f.m.container_id(), 0u);
    f.c = {};
    f.m.send_changes(out, kNoCompression);
    // Only what the own menu never told: its 10 slots that the chest menu does not show
    // (result, grid, armour, off hand) - plus the slot the cursor went back into, and the cursor.
    const auto sent = packets(out);
    u32 slots = 0;
    for (const Sent& s : sent) slots += s.id == ids::play::cb::kContainerSetSlot;
    EXPECT_EQ(slots, 10u + 1u);
}

TEST(ContainerMenu, DoubleChestIsTwoContainers) {
    Fixture f;
    f.chest[1].items[0] = ItemStack(it::kDirt, 1);
    f.open_chest(6);
    EXPECT_EQ(f.m.slot_count(), 90);
    EXPECT_EQ(f.at(27), is(it::kDirt, 1));  // the second half begins at slot 27
    f.m.inv.items[9] = ItemStack(it::kStone, 3);
    EXPECT_EQ(f.at(54), is(it::kStone, 3));
    EXPECT_TRUE(f.shift(54));
    EXPECT_EQ(is(f.chest[0].items[0]), is(it::kStone, 3));
    // A drag over slots of both halves and of the inventory, 80 and above.
    f.m.carried = ItemStack(it::kStone, 9);
    EXPECT_TRUE(f.click(-999, 0, C::kQuickCraft));
    for (const i32 slot : {1, 53, 89}) EXPECT_TRUE(f.click(slot, 1, C::kQuickCraft));
    EXPECT_TRUE(f.click(-999, 2, C::kQuickCraft));
    EXPECT_EQ(is(f.chest[0].items[1]), is(it::kStone, 3));
    EXPECT_EQ(is(f.chest[1].items[26]), is(it::kStone, 3));
    EXPECT_EQ(is(f.m.inv.items[8]), is(it::kStone, 3));
    EXPECT_TRUE(f.m.carried.empty());
}

TEST(ContainerMenu, FurnaceSlots) {
    Fixture f;
    f.m.inv.items[0] = ItemStack(it::kCoal, 8);
    f.m.inv.items[1] = ItemStack(it::kCobblestone, 20);
    f.m.inv.items[2] = ItemStack(it::kDirt, 4);
    f.m.inv.items[3] = ItemStack(it::kBucket, 16);
    f.open_furnace();
    EXPECT_EQ(f.m.slot_count(), 39);
    // AbstractFurnaceMenu.quickMoveStack: what smelts goes to the input, what burns to the fuel,
    // everything else between the hotbar and the rest of the inventory.
    EXPECT_TRUE(f.shift(30));
    EXPECT_EQ(is(f.furnace.items[Furnace::kFuel]), is(it::kCoal, 8));
    EXPECT_TRUE(f.shift(31));
    EXPECT_EQ(is(f.furnace.items[Furnace::kInput]), is(it::kCobblestone, 20));
    EXPECT_EQ(f.furnace.cook_total, 200);  // Slot.set reached the furnace's setItem
    EXPECT_TRUE(f.shift(32));
    EXPECT_EQ(is(f.m.inv.items[9]), is(it::kDirt, 4));
    EXPECT_TRUE(f.furnace.items[Furnace::kResult].empty());

    // The fuel slot takes fuel and a single empty bucket; the result slot takes nothing.
    f.m.carried = ItemStack(it::kDirt, 1);
    EXPECT_TRUE(f.left(1));
    EXPECT_EQ(is(f.m.carried), is(it::kDirt, 1));
    EXPECT_TRUE(f.left(2));
    EXPECT_EQ(is(f.m.carried), is(it::kDirt, 1));
    f.m.carried = {};
    f.furnace.items[Furnace::kFuel] = {};
    EXPECT_TRUE(f.left(33));  // the buckets
    EXPECT_TRUE(f.left(1));
    EXPECT_EQ(is(f.furnace.items[Furnace::kFuel]), is(it::kBucket, 1));
    EXPECT_EQ(is(f.m.carried), is(it::kBucket, 15));
    EXPECT_TRUE(f.left(33));

    // The result comes out by a click or a shift-click, into the hotbar's far end first.
    f.furnace.items[Furnace::kResult] = ItemStack(it::kStone, 7);
    EXPECT_TRUE(f.shift(2));
    EXPECT_EQ(is(f.m.inv.items[8]), is(it::kStone, 7));

    // What the menu shows of the fire: four numbers, sent whole and then as they change.
    base::ByteBuf out;
    f.furnace.lit_remaining = 77;
    f.furnace.lit_total = 1600;
    f.m.send_all(out, kNoCompression, f.c);
    auto sent = packets(out);
    ASSERT_EQ(sent.size(), 5u);
    EXPECT_EQ(sent[0].id, ids::play::cb::kContainerSetContent);
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_EQ(sent[1 + i].id, ids::play::cb::kContainerSetData);
        Reader r(sent[1 + i].body.data(), sent[1 + i].body.size());
        EXPECT_EQ(r.varint(), 2u);  // the container id
        EXPECT_EQ(r.be16(), i);
        EXPECT_EQ(static_cast<i16>(r.be16()), i == 0 ? 77 : i == 1 ? 1600 : i == 3 ? 200 : 0);
        EXPECT_TRUE(r.done());
    }
    f.m.send_changes(out, kNoCompression, f.c);
    EXPECT_EQ(out.len, 0u);
    f.furnace.lit_remaining = 76;
    f.m.send_changes(out, kNoCompression, f.c);
    sent = packets(out);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].id, ids::play::cb::kContainerSetData);
}

TEST(ContainerMenu, CraftingTable) {
    Fixture f;
    f.m.inv.items[0] = ItemStack(it::kOakPlanks, 64);
    f.open_table();
    EXPECT_EQ(f.m.slot_count(), 46);
    EXPECT_EQ(f.at(37), is(it::kOakPlanks, 64));
    // Planks onto the cursor, one into each of two cells on top of each other: sticks.
    EXPECT_TRUE(f.left(37));
    EXPECT_TRUE(f.right(1));
    EXPECT_EQ(f.results(), (std::vector<Is>{is(it::kOakButton, 1)}));  // one plank alone is a button
    EXPECT_TRUE(f.right(4));
    EXPECT_EQ(f.results(), (std::vector<Is>{is(it::kStick, 4)}));
    EXPECT_EQ(f.at(0), is(it::kStick, 4));
    EXPECT_TRUE(f.left(37));  // the rest of the planks back
    // Taking the result uses one of each ingredient - and the grid says what it makes after
    // every one of them (TransientCraftingContainer.removeItem): two cells, two packets, the
    // first of them about the plank that is left for a moment.
    EXPECT_TRUE(f.left(0));
    EXPECT_EQ(is(f.m.carried), is(it::kStick, 4));
    EXPECT_EQ(f.results(), (std::vector<Is>{is(it::kOakButton, 1), kNothing}));
    EXPECT_EQ(f.at(1), kNothing);
    EXPECT_EQ(f.at(4), kNothing);
    EXPECT_EQ(f.at(0), kNothing);
    f.m.carried = {};

    // Shift-click on the result crafts for as long as the grid makes the same thing.
    f.m.carried = ItemStack(it::kOakPlanks, 6);
    EXPECT_TRUE(f.click(-999, 0, C::kQuickCraft));  // a drag: three planks each in two cells
    EXPECT_TRUE(f.click(2, 1, C::kQuickCraft));
    EXPECT_TRUE(f.click(5, 1, C::kQuickCraft));
    EXPECT_TRUE(f.click(-999, 2, C::kQuickCraft));
    EXPECT_EQ(f.at(0), is(it::kStick, 4));
    f.fx.clear();
    EXPECT_TRUE(f.shift(0));
    EXPECT_EQ(f.at(0), kNothing);
    EXPECT_EQ(f.at(2), kNothing);
    u32 sticks = 0;
    for (u32 i = 0; i < inv::kItems; ++i) sticks += f.m.inv.items[i].item() == it::kStick ? f.m.inv.items[i].count() : 0;
    EXPECT_EQ(sticks, 12u);

    // What an ingredient leaves behind stays in its cell: a cake's three buckets.
    f.fx.clear();
    const u32 cake[9] = {it::kMilkBucket, it::kMilkBucket, it::kMilkBucket, it::kSugar, it::kEgg,
                         it::kSugar,      it::kWheat,      it::kWheat,      it::kWheat};
    for (i32 k = 0; k < 9; ++k) {
        f.m.carried = ItemStack(cake[k], 1);
        EXPECT_TRUE(f.left(1 + k));
    }
    EXPECT_EQ(f.at(0), is(it::kCake, 1));
    EXPECT_TRUE(f.left(0));
    EXPECT_EQ(is(f.m.carried), is(it::kCake, 1));
    EXPECT_EQ(f.at(1), is(it::kBucket, 1));
    EXPECT_EQ(f.at(3), is(it::kBucket, 1));
    EXPECT_EQ(f.at(4), kNothing);
    EXPECT_EQ(f.at(0), kNothing);

    // Closing hands the grid back (CraftingMenu.removed) along with the cursor.
    f.m.close(f.cx());
    u32 buckets = 0;
    for (u32 i = 0; i < inv::kItems; ++i) buckets += f.m.inv.items[i].item() == it::kBucket ? f.m.inv.items[i].count() : 0;
    EXPECT_EQ(buckets, 3u);
    EXPECT_EQ(f.m.kind(), Menu::Kind::kInventory);
}

TEST(ContainerMenu, TheInventorysOwnGrid) {
    // InventoryMenu: the same result slot over a 2x2 grid, container id 0.
    Menu m;
    Effects fx;
    const Menu::Ctx cx{false, fx};
    m.inv.items[0] = ItemStack(it::kOakLog, 3);
    EXPECT_TRUE(m.click(36, 0, C::kPickup, cx));
    EXPECT_TRUE(m.click(1, 0, C::kPickup, cx));
    ASSERT_EQ(fx.size(), 1u);
    EXPECT_EQ(fx[0].kind, Effect::kSetSlot);
    EXPECT_EQ(fx[0].slot, 0);  // the container id
    EXPECT_EQ(is(fx[0].stack), is(it::kOakPlanks, 4));
    EXPECT_EQ(is(m.at(0)), is(it::kOakPlanks, 4));
    fx.clear();
    EXPECT_TRUE(m.click(0, 0, C::kQuickMove, cx));  // three logs, twelve planks
    EXPECT_TRUE(m.at(0).empty());
    EXPECT_TRUE(m.at(1).empty());
    u32 planks = 0;
    for (u32 i = 0; i < inv::kItems; ++i) planks += m.inv.items[i].item() == it::kOakPlanks ? m.inv.items[i].count() : 0;
    EXPECT_EQ(planks, 12u);
    // The result cannot be put back, and a double click does not gather it.
    m.craft[0] = ItemStack(it::kOakLog, 1);
    m.carried = ItemStack(it::kOakPlanks, 1);
    EXPECT_TRUE(m.click(1, 0, C::kPickup, cx));  // swaps the log out: a plank alone makes a button
    m.carried = ItemStack(it::kStone, 1);
    const Is before = is(m.at(0));
    EXPECT_TRUE(m.click(0, 0, C::kPickup, cx));
    EXPECT_EQ(is(m.at(0)), before);
    EXPECT_EQ(is(m.carried), is(it::kStone, 1));
}
