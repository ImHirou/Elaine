// SPDX-License-Identifier: GPL-3.0-or-later
// Item stacks and component patches: what a client may send, what is kept of it, and what a
// stack answers about itself. The byte strings are written out by hand from the components'
// StreamCodecs in the 26.3 jar, not produced by the code under test.
#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "item/codec.hpp"
#include "item/stack.hpp"
#include "nbt/snbt.hpp"
#include "nbt_bytes.hpp"
#include "proto/packets.hpp"

#include PROTO_GEN_ENTITIES_HPP
#include PROTO_GEN_ITEMS_HPP

using namespace proto;
using item::ItemStack;
using item::PatchId;
using item::Verdict;
namespace comp = proto::gen::component;
namespace it = proto::gen::item;
namespace n = nbt_bytes;

namespace {

using Bytes = std::vector<u8>;

Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
Bytes vi(u32 v) {
    u8 buf[8];
    return {buf, put_varint(buf, v)};
}
Bytes be32(u32 v) { return {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)}; }
Bytes f32(float v) { return be32(std::bit_cast<u32>(v)); }
Bytes str(std::string_view s) { return vi(static_cast<u32>(s.size())) + Bytes(s.begin(), s.end()); }

struct Added {
    u32 type;
    Bytes value;
};
// DataComponentPatch.DELIMITED_STREAM_CODEC: counts, (type, length, value)..., removed types.
Bytes untrusted_patch(std::initializer_list<Added> added, std::initializer_list<u32> removed = {}) {
    Bytes out = vi(static_cast<u32>(added.size())) + vi(static_cast<u32>(removed.size()));
    for (const Added& a : added) out = out + vi(a.type) + vi(static_cast<u32>(a.value.size())) + a.value;
    for (const u32 type : removed) out = out + vi(type);
    return out;
}
// DataComponentPatch.STREAM_CODEC: the same without the lengths.
Bytes trusted_patch(std::initializer_list<Added> added, std::initializer_list<u32> removed = {}) {
    Bytes out = vi(static_cast<u32>(added.size())) + vi(static_cast<u32>(removed.size()));
    for (const Added& a : added) out = out + vi(a.type) + a.value;
    for (const u32 type : removed) out = out + vi(type);
    return out;
}

struct Read {
    Verdict verdict;
    Bytes wire;     // what the patch became, as it would be sent to a client
    bool consumed;  // the reader stopped exactly at the end
};
Read read_patch(const Bytes& in) {
    Reader r(in.data(), in.size());
    PatchId id = 0;
    Read out{item::patches().read_untrusted(r, id), {}, r.done()};
    if (id) {
        const auto w = item::patches().wire(id);
        out.wire.assign(w.begin(), w.end());
        item::patches().release(id);
    }
    return out;
}
// One component with this value: the verdict, and (kOk) the value as it is kept.
Verdict read_value(u32 type, const Bytes& value, Bytes* kept = nullptr) {
    const Read r = read_patch(untrusted_patch({{type, value}}));
    if (r.verdict == Verdict::kOk && kept) {
        const Bytes head = vi(1) + vi(0) + vi(type);
        EXPECT_GE(r.wire.size(), head.size());
        kept->assign(r.wire.begin() + static_cast<long>(head.size()), r.wire.end());
    }
    return r.verdict;
}
void expect_kept(u32 type, const Bytes& sent, const Bytes& expected) {
    Bytes kept;
    EXPECT_EQ(read_value(type, sent, &kept), Verdict::kOk) << "component " << type;
    EXPECT_EQ(kept, expected) << "component " << type;
}

ItemStack with_patch(u32 item_id, u32 count, const Bytes& patch) {
    Bytes in = vi(count) + vi(item_id) + patch;
    Reader r(in.data(), in.size());
    ItemStack s;
    EXPECT_EQ(item::read_untrusted(r, s), Verdict::kOk);
    EXPECT_TRUE(r.done());
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------- stacks

TEST(ItemStack, EmptyIsAirOrNothing) {
    static_assert(sizeof(ItemStack) == 8);
    EXPECT_TRUE(ItemStack().empty());
    EXPECT_TRUE(ItemStack(it::kAir, 5).empty());
    EXPECT_TRUE(ItemStack(it::kStone, 0).empty());
    const ItemStack stone(it::kStone, 3);
    EXPECT_FALSE(stone.empty());
    EXPECT_EQ(stone.item(), it::kStone);
    EXPECT_EQ(stone.count(), 3u);
    // An emptied stack answers like no stack at all...
    ItemStack gone = stone;
    gone.shrink(3);
    EXPECT_TRUE(gone.empty());
    EXPECT_EQ(gone.item(), 0u);
    EXPECT_EQ(gone.count(), 0u);
    EXPECT_TRUE(item::matches(gone, ItemStack()));
    EXPECT_TRUE(item::same_item_same_components(gone, ItemStack(it::kDirt, 0)));
    EXPECT_FALSE(item::same_item(gone, stone));
}

TEST(ItemStack, SplitGrowAndCompare) {
    ItemStack a(it::kStone, 10);
    ItemStack b = a.split(4);
    EXPECT_EQ(a.count(), 6u);
    EXPECT_EQ(b.count(), 4u);
    EXPECT_TRUE(item::same_item_same_components(a, b));
    EXPECT_FALSE(item::matches(a, b));
    b.grow(2);
    EXPECT_TRUE(item::matches(a, b));
    EXPECT_EQ(a.split(99).count(), 6u);  // no more than there is
    EXPECT_TRUE(a.empty());
    EXPECT_TRUE(a.split(1).empty());
    EXPECT_FALSE(item::same_item(b, ItemStack(it::kDirt, 6)));
    ItemStack c = b.take();
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(c.count(), 6u);
    EXPECT_EQ(c.with_count(1).count(), 1u);
    EXPECT_TRUE(ItemStack().with_count(5).empty());
}

TEST(ItemStack, DefaultsComeFromTheItem) {
    EXPECT_EQ(ItemStack(it::kStone, 1).max_stack(), 64u);
    EXPECT_EQ(ItemStack(it::kEnderPearl, 1).max_stack(), 16u);
    EXPECT_EQ(ItemStack(it::kDiamondSword, 1).max_stack(), 1u);
    EXPECT_TRUE(ItemStack(it::kStone, 1).stackable());
    EXPECT_FALSE(ItemStack(it::kDiamondSword, 1).stackable());
    EXPECT_TRUE(ItemStack(it::kDiamondSword, 1).damageable());
    EXPECT_FALSE(ItemStack(it::kDiamondSword, 1).damaged());
    EXPECT_FALSE(ItemStack(it::kStone, 1).damageable());
    EXPECT_FALSE(ItemStack(it::kStone, 1).enchanted());

    namespace equip = item::equip;
    EXPECT_EQ(ItemStack(it::kDiamondHelmet, 1).equip_slot(), equip::kHead);
    EXPECT_EQ(ItemStack(it::kElytra, 1).equip_slot(), equip::kChest);
    EXPECT_EQ(ItemStack(it::kIronLeggings, 1).equip_slot(), equip::kLegs);
    EXPECT_EQ(ItemStack(it::kLeatherBoots, 1).equip_slot(), equip::kFeet);
    EXPECT_EQ(ItemStack(it::kShield, 1).equip_slot(), equip::kOffhand);
    EXPECT_EQ(ItemStack(it::kCarvedPumpkin, 1).equip_slot(), equip::kHead);
    EXPECT_EQ(ItemStack(it::kStone, 1).equip_slot(), item::kNoEquipSlot);
    EXPECT_EQ(ItemStack(it::kDiamondHelmet, 0).equip_slot(), item::kNoEquipSlot);  // an empty stack is nothing
    EXPECT_TRUE(ItemStack(it::kDiamondHelmet, 1).players_may_wear());
    // Horse armour goes on a body, but not on a player's.
    EXPECT_EQ(ItemStack(it::kDiamondHorseArmor, 1).equip_slot(), equip::kBody);
    EXPECT_FALSE(ItemStack(it::kDiamondHorseArmor, 1).players_may_wear());
    EXPECT_TRUE(item::humanoid_armor(equip::kFeet) && item::humanoid_armor(equip::kHead));
    EXPECT_FALSE(item::humanoid_armor(equip::kOffhand) || item::humanoid_armor(equip::kBody) ||
                 item::humanoid_armor(equip::kMainhand));

    // getAttackAnimation: a whack of 6 ticks, except for the spears - each a stab of its own length.
    namespace swing = proto::swing_animation;
    const auto attack = [](u32 item_id, u32 count = 1) {
        const proto::Swing s = ItemStack(item_id, count).attack_swing();
        return std::pair<u32, u32>(s.type, s.ticks);
    };
    EXPECT_EQ(attack(it::kStone), std::pair(swing::kWhack, 6u));
    EXPECT_EQ(attack(it::kDiamondSword), std::pair(swing::kWhack, 6u));
    EXPECT_EQ(attack(it::kWoodenSpear), std::pair(swing::kStab, 13u));
    EXPECT_EQ(attack(it::kNetheriteSpear), std::pair(swing::kStab, 23u));
    EXPECT_EQ(attack(it::kWoodenSpear, 0), std::pair(swing::kWhack, 6u));  // an empty hand
    const ItemStack none;
    EXPECT_EQ(none.attack_swing().type, swing::kWhack);
}

// ---------------------------------------------------------------------------- patches

TEST(Patches, EmptyPatchIsNoPatch) {
    const u32 live = item::patches().live();
    const Read r = read_patch(untrusted_patch({}));
    EXPECT_EQ(r.verdict, Verdict::kOk);
    EXPECT_TRUE(r.wire.empty());
    EXPECT_TRUE(r.consumed);
    EXPECT_EQ(item::patches().live(), live);
}

TEST(Patches, StoredAsTheTrustedEncodingSortedByType) {
    // Sent out of order, with a value length in front of each value.
    const Bytes sent = untrusted_patch({{comp::kRepairCost, vi(7)}, {comp::kDamage, vi(300)}, {comp::kUnbreakable, {}}},
                                       {comp::kLore, comp::kCustomName});
    const Read r = read_patch(sent);
    ASSERT_EQ(r.verdict, Verdict::kOk);
    EXPECT_TRUE(r.consumed);
    EXPECT_EQ(r.wire, trusted_patch({{comp::kDamage, vi(300)}, {comp::kUnbreakable, {}}, {comp::kRepairCost, vi(7)}},
                                    {comp::kCustomName, comp::kLore}));
}

TEST(Patches, EqualPatchesAreOnePatch) {
    const u32 live = item::patches().live();
    const Bytes one = untrusted_patch({{comp::kDamage, vi(5)}, {comp::kRepairCost, vi(1)}});
    const Bytes other =
        untrusted_patch({{comp::kRepairCost, vi(1)}, {comp::kDamage, {0x85, 0x00}}});  // reordered, a padded VarInt
    {
        const ItemStack a = with_patch(it::kDiamondSword, 1, one);
        const ItemStack b = with_patch(it::kDiamondSword, 1, other);
        const ItemStack c = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(6)}}));
        EXPECT_NE(a.patch(), 0u);
        EXPECT_EQ(a.patch(), b.patch());
        EXPECT_NE(a.patch(), c.patch());
        EXPECT_TRUE(item::matches(a, b));
        EXPECT_FALSE(item::same_item_same_components(a, c));
        EXPECT_FALSE(item::same_item_same_components(a, ItemStack(it::kDiamondSword, 1)));
        EXPECT_EQ(item::patches().live(), live + 2);
        const ItemStack copy = a;  // a third holder of the first patch
        EXPECT_EQ(copy.patch(), a.patch());
    }
    EXPECT_EQ(item::patches().live(), live);  // the last holder gone, the patch is gone
}

TEST(Patches, ManyDistinctPatchesSurviveGrowthAndReuse) {
    const u32 live = item::patches().live();
    std::vector<ItemStack> held;
    for (u32 i = 0; i < 500; ++i) held.push_back(with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(i + 1)}})));
    EXPECT_EQ(item::patches().live(), live + 500);
    for (u32 i = 0; i < 500; i += 2) held[i] = {};  // free every other one: holes in the table
    EXPECT_EQ(item::patches().live(), live + 250);
    for (u32 i = 1; i < 500; i += 2) {
        const ItemStack again = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(i + 1)}}));
        EXPECT_EQ(again.patch(), held[i].patch()) << i;  // still found after the removals around it
    }
    for (u32 i = 0; i < 500; i += 2) held[i] = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(i + 1)}}));
    EXPECT_EQ(item::patches().live(), live + 500);
    for (u32 i = 0; i < 500; ++i) {
        const auto found = item::patches().find(held[i].patch(), comp::kDamage);
        ASSERT_EQ(found.state, item::Patches::State::kSet);
        EXPECT_EQ(Bytes(found.value.begin(), found.value.end()), vi(i + 1));
    }
    held.clear();
    EXPECT_EQ(item::patches().live(), live);
}

TEST(Patches, LaterEntryWinsAndRemovalBeatsValue) {
    // Vanilla reads the entries into a map: values first, removals after.
    Read r = read_patch(untrusted_patch({{comp::kDamage, vi(1)}, {comp::kDamage, vi(2)}}));
    ASSERT_EQ(r.verdict, Verdict::kOk);
    EXPECT_EQ(r.wire, trusted_patch({{comp::kDamage, vi(2)}}));
    r = read_patch(untrusted_patch({{comp::kDamage, vi(1)}}, {comp::kDamage}));
    ASSERT_EQ(r.verdict, Verdict::kOk);
    EXPECT_EQ(r.wire, trusted_patch({}, {comp::kDamage}));
}

TEST(Patches, FindSaysSetRemovedOrInherited) {
    const ItemStack s = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(9)}}, {comp::kMaxStackSize}));
    using State = item::Patches::State;
    EXPECT_EQ(item::patches().find(s.patch(), comp::kDamage).state, State::kSet);
    EXPECT_EQ(item::patches().find(s.patch(), comp::kMaxStackSize).state, State::kRemoved);
    EXPECT_EQ(item::patches().find(s.patch(), comp::kRepairCost).state, State::kInherit);
    EXPECT_EQ(item::patches().entries(s.patch()).size(), 2u);
}

TEST(Patches, EnvelopeErrorsAreMalformed) {
    EXPECT_EQ(read_patch({}).verdict, Verdict::kMalformed);                      // no counts
    EXPECT_EQ(read_patch(vi(1) + vi(0)).verdict, Verdict::kMalformed);           // an entry promised, none there
    EXPECT_EQ(read_patch(vi(0xFFFFFFFF) + vi(0)).verdict, Verdict::kMalformed);  // a negative count
    EXPECT_EQ(read_patch(vi(1) + vi(0) + vi(comp::kCount) + vi(0)).verdict, Verdict::kMalformed);  // no such component type
    EXPECT_EQ(read_patch(vi(0) + vi(1) + vi(comp::kCount)).verdict, Verdict::kMalformed);          // ...not to remove either
    EXPECT_EQ(read_patch(vi(1) + vi(0) + vi(comp::kDamage) + vi(5) + vi(1)).verdict,
              Verdict::kMalformed);  // value shorter than its length
    EXPECT_EQ(read_patch(vi(1) + vi(0) + vi(comp::kDamage) + vi(0)).verdict, Verdict::kMalformed);  // a VarInt in zero bytes
}

TEST(Patches, WhatTheServerCannotCheckIsUnsupportedNotWrong) {
    // tool: what an item does. Well-formed here or not, it is not this server's to say.
    Read r = read_patch(untrusted_patch({{comp::kTool, vi(0)}}));
    EXPECT_EQ(r.verdict, Verdict::kUnsupported);
    EXPECT_TRUE(r.consumed);  // the length says where it ends: what follows the stack can still be read
    EXPECT_TRUE(r.wire.empty());
    // One unsupported component spoils the stack, whatever else it carries...
    EXPECT_EQ(read_patch(untrusted_patch({{comp::kDamage, vi(1)}, {comp::kFood, {0x01, 0x02, 0x03}}})).verdict,
              Verdict::kUnsupported);
    // ...but removing a component needs no understanding of it.
    EXPECT_EQ(read_patch(untrusted_patch({}, {comp::kTool, comp::kAttributeModifiers})).verdict, Verdict::kOk);
    // Malformed beats unsupported when it is noticed.
    EXPECT_EQ(read_patch(untrusted_patch({{comp::kTool, {0x08}}, {comp::kDamage, vi(0xFFFFFFFF)}})).verdict, Verdict::kMalformed);
}

TEST(Patches, TooLargeIsUnsupported) {
    // 400 custom potion effects: more than the creative inventory ever sends, and kept.
    Bytes effects = vi(400);
    for (u32 i = 0; i < 400; ++i) effects = effects + vi(i % 40) + Bytes{0, 1, 0, 1, 1, 0};
    const Bytes potion = Bytes{0, 0} + effects + Bytes{0};
    EXPECT_EQ(read_patch(untrusted_patch({{comp::kPotionContents, potion}})).verdict, Verdict::kOk);
    // Custom model data of 300 000 floats: fine for vanilla, more than a stack may carry here.
    Bytes model = vi(300000);
    model.resize(model.size() + 4 * 300000);
    model = model + vi(0) + vi(0) + vi(0);
    ASSERT_GT(model.size(), item::kMaxPatchBytes);
    const Read r = read_patch(untrusted_patch({{comp::kCustomModelData, model}}));
    EXPECT_EQ(r.verdict, Verdict::kUnsupported);
    EXPECT_TRUE(r.consumed);
    // ...and 250 000 are within it: a heavy stack, which is counted where it costs.
    Bytes fits = vi(250000);
    fits.resize(fits.size() + 4 * 250000);
    fits = vi(1) + vi(it::kStone) + untrusted_patch({{comp::kCustomModelData, fits + vi(0) + vi(0) + vi(0)}});
    Reader in(fits.data(), fits.size());
    ItemStack heavy;
    ASSERT_EQ(item::read_untrusted(in, heavy), Verdict::kOk);
    EXPECT_GT(item::heavy_bytes(heavy), 1000000u);
    EXPECT_EQ(item::heavy_bytes(ItemStack(it::kStone, 1)), 0u);
}

// ---------------------------------------------------------------------------- component values

TEST(Components, IntegersWithRanges) {
    expect_kept(comp::kMaxStackSize, vi(1), vi(1));
    expect_kept(comp::kMaxStackSize, vi(99), vi(99));
    EXPECT_EQ(read_value(comp::kMaxStackSize, vi(0)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kMaxStackSize, vi(100)), Verdict::kMalformed);
    expect_kept(comp::kMaxDamage, vi(1561), vi(1561));
    EXPECT_EQ(read_value(comp::kMaxDamage, vi(0)), Verdict::kMalformed);  // POSITIVE_INT
    expect_kept(comp::kDamage, vi(0), vi(0));
    EXPECT_EQ(read_value(comp::kDamage, vi(0xFFFFFFFF)), Verdict::kMalformed);  // -1: NON_NEGATIVE_INT
    expect_kept(comp::kRepairCost, vi(0x7FFFFFFF), vi(0x7FFFFFFF));
    EXPECT_EQ(read_value(comp::kRepairCost, vi(0x80000000)), Verdict::kMalformed);
    expect_kept(comp::kOminousBottleAmplifier, vi(4), vi(4));
    EXPECT_EQ(read_value(comp::kOminousBottleAmplifier, vi(5)), Verdict::kMalformed);
    expect_kept(comp::kMapId, vi(0xFFFFFFFF), vi(0xFFFFFFFF));  // any int
    // What vanilla would write, not what was sent: a padded VarInt, bytes after the value.
    expect_kept(comp::kDamage, {0x83, 0x80, 0x00}, vi(3));
    expect_kept(comp::kDamage, {0x03, 0xAA, 0xBB}, vi(3));
}

TEST(Components, UnitsBoolsEnumsFloats) {
    expect_kept(comp::kUnbreakable, {}, {});
    expect_kept(comp::kGlider, {0x01, 0x02}, {});  // lengthPrefixed drops what the codec does not read
    expect_kept(comp::kWaxed, {}, {});
    expect_kept(comp::kEnchantmentGlintOverride, {0x00}, {0x00});
    expect_kept(comp::kEnchantmentGlintOverride, {0x7F}, {0x01});  // readBoolean: anything but zero
    EXPECT_EQ(read_value(comp::kEnchantmentGlintOverride, {}), Verdict::kMalformed);
    // ByIdMap with OutOfBoundsStrategy.ZERO: an id outside the enum is its first value.
    expect_kept(comp::kRarity, vi(3), vi(3));
    expect_kept(comp::kRarity, vi(4), vi(0));
    expect_kept(comp::kDye, vi(15), vi(15));
    expect_kept(comp::kBaseColor, vi(16), vi(0));
    expect_kept(comp::kCushionColor, vi(0xFFFFFFFF), vi(0));
    expect_kept(comp::kDyedColor, be32(0xFFA06540), be32(0xFFA06540));
    EXPECT_EQ(read_value(comp::kDyedColor, {1, 2, 3}), Verdict::kMalformed);
    // Float ranges are Float.compareTo's: NaN is above everything, -0.0 is below 0.0.
    expect_kept(comp::kMinimumAttackCharge, f32(0.0f), f32(0.0f));
    expect_kept(comp::kMinimumAttackCharge, f32(1.0f), f32(1.0f));
    EXPECT_EQ(read_value(comp::kMinimumAttackCharge, f32(1.5f)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kMinimumAttackCharge, f32(-0.0f)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kMinimumAttackCharge, f32(std::numeric_limits<float>::quiet_NaN())), Verdict::kMalformed);
    expect_kept(comp::kPotionDurationScale, f32(3.4028235e38f), f32(3.4028235e38f));
    EXPECT_EQ(read_value(comp::kPotionDurationScale, f32(std::numeric_limits<float>::infinity())), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kPotionDurationScale, f32(-1.0f)), Verdict::kMalformed);
}

TEST(Components, IdentifiersAreParsedAndWrittenInFull) {
    expect_kept(comp::kItemModel, str("minecraft:stone"), str("minecraft:stone"));
    expect_kept(comp::kItemModel, str("stone"), str("minecraft:stone"));  // Identifier.parse adds the namespace
    expect_kept(comp::kItemModel, str(":stone"), str("minecraft:stone"));
    expect_kept(comp::kTooltipStyle, str("my-pack:a/b.c_d"), str("my-pack:a/b.c_d"));
    expect_kept(comp::kNoteBlockSound, str("a:"), str("a:"));                    // an empty path is a path
    EXPECT_EQ(read_value(comp::kItemModel, str("Stone")), Verdict::kMalformed);  // upper case
    EXPECT_EQ(read_value(comp::kItemModel, str("a:b:c")), Verdict::kMalformed);  // ':' is not a path character
    EXPECT_EQ(read_value(comp::kItemModel, str("a/b:c")), Verdict::kMalformed);  // '/' is not a namespace character
    EXPECT_EQ(read_value(comp::kItemModel, str("..:c")), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kItemModel, str("caf\xC3\xA9")), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kItemModel, vi(5) + Bytes{'a', 'b'}), Verdict::kMalformed);  // shorter than it says
}

TEST(Components, EnchantmentsAreAMapWithLevels) {
    namespace ench = proto::reg::enchantment;
    // {sharpness: 5, unbreaking: 3}, sent in the other order and with a repeated key.
    const Bytes sent = vi(3) + vi(ench::kUnbreaking) + vi(1) + vi(ench::kSharpness) + vi(5) + vi(ench::kUnbreaking) + vi(3);
    const u32 lo = std::min(ench::kSharpness, ench::kUnbreaking), hi = std::max(ench::kSharpness, ench::kUnbreaking);
    const Bytes kept = vi(2) + vi(lo) + vi(lo == ench::kSharpness ? 5 : 3) + vi(hi) + vi(hi == ench::kSharpness ? 5 : 3);
    expect_kept(comp::kEnchantments, sent, kept);
    expect_kept(comp::kStoredEnchantments, sent, kept);
    expect_kept(comp::kEnchantments, vi(0), vi(0));
    EXPECT_EQ(read_value(comp::kEnchantments, vi(1) + vi(ench::kCount) + vi(1)), Verdict::kMalformed);  // no such enchantment
    EXPECT_EQ(read_value(comp::kEnchantments, vi(1) + vi(0) + vi(0)), Verdict::kMalformed);             // level 0
    EXPECT_EQ(read_value(comp::kEnchantments, vi(1) + vi(0) + vi(256)), Verdict::kMalformed);           // level 256
    EXPECT_EQ(read_value(comp::kEnchantments, vi(2) + vi(0) + vi(1)), Verdict::kMalformed);             // one entry short
    EXPECT_EQ(read_value(comp::kEnchantments, vi(0xFFFFFFFF)), Verdict::kMalformed);                    // a negative count

    const ItemStack book = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kEnchantments, sent}}));
    EXPECT_TRUE(book.enchanted());
    EXPECT_FALSE(with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kEnchantments, vi(0)}})).enchanted());
    EXPECT_FALSE(with_patch(it::kEnchantedBook, 1, untrusted_patch({{comp::kStoredEnchantments, sent}}))
                     .enchanted());  // stored is not applied
}

TEST(Components, PotionContents) {
    namespace sreg = proto::gen::static_registry;
    // A potion id, no colour, no custom effects, no name: what the creative inventory sends.
    expect_kept(comp::kPotionContents, Bytes{1} + vi(7) + Bytes{0} + vi(0) + Bytes{0},
                Bytes{1} + vi(7) + Bytes{0} + vi(0) + Bytes{0});
    // Everything set: colour, one effect with a hidden effect below it, a name. The hidden
    // effect is read and dropped, as a vanilla server was seen to (component_probe.py).
    const Bytes effect = vi(3) /* effect */ + vi(300) /* amplifier, clamped */ + vi(600) + Bytes{1, 1, 0} +
                         Bytes{1} /* hidden */ + vi(1) + vi(0xFFFFFFFF) /* infinite */ + Bytes{0, 9, 1} + Bytes{0};
    const Bytes kept = vi(3) + vi(255) + vi(600) + Bytes{1, 1, 0} + Bytes{0};
    expect_kept(comp::kPotionContents, Bytes{0} + Bytes{1} + be32(0x00FF8800) + vi(1) + effect + Bytes{1} + str("Brew"),
                Bytes{0} + Bytes{1} + be32(0x00FF8800) + vi(1) + kept + Bytes{1} + str("Brew"));
    EXPECT_EQ(read_value(comp::kPotionContents, Bytes{1} + vi(sreg::kPotionCount) + Bytes{0} + vi(0) + Bytes{0}),
              Verdict::kMalformed);
    EXPECT_EQ(
        read_value(comp::kPotionContents, Bytes{0, 0} + vi(1) + vi(sreg::kMobEffectCount) + Bytes{0, 0, 0, 0, 0, 0} + Bytes{0}),
        Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kPotionContents, Bytes{0, 0} + vi(0)), Verdict::kMalformed);  // ends before the name flag
    // Hidden effects nested deeper than anything real: refused, not followed.
    Bytes deep = Bytes{0, 0} + vi(1) + vi(0);
    for (int i = 0; i < 40; ++i) deep = deep + Bytes{0, 0, 0, 0, 0, 1};
    deep = deep + Bytes{0, 0, 0, 0, 0, 0} + Bytes{0};
    EXPECT_EQ(read_value(comp::kPotionContents, deep), Verdict::kUnsupported);
}

TEST(Components, StewFireworksAndLists) {
    expect_kept(comp::kSuspiciousStewEffects, vi(2) + vi(1) + vi(160) + vi(5) + vi(7), vi(2) + vi(1) + vi(160) + vi(5) + vi(7));
    EXPECT_EQ(read_value(comp::kSuspiciousStewEffects, vi(1) + vi(40) + vi(1)), Verdict::kMalformed);

    const Bytes star =
        vi(9) /* shape: out of range */ + vi(2) + be32(0xFF0000) + be32(0x00FF00) + vi(1) + be32(0x0000FF) + Bytes{5, 0};
    const Bytes star_kept = vi(0) + vi(2) + be32(0xFF0000) + be32(0x00FF00) + vi(1) + be32(0x0000FF) + Bytes{1, 0};
    expect_kept(comp::kFireworkExplosion, star, star_kept);
    expect_kept(comp::kFireworks, vi(3) + vi(2) + star + star, vi(3) + vi(2) + star_kept + star_kept);
    expect_kept(comp::kFireworks, vi(1) + vi(0), vi(1) + vi(0));                    // a plain rocket
    EXPECT_EQ(read_value(comp::kFireworks, vi(256) + vi(0)), Verdict::kMalformed);  // flight duration is an unsigned byte
    EXPECT_EQ(read_value(comp::kFireworks, vi(1) + vi(257)), Verdict::kMalformed);  // at most 256 explosions
    EXPECT_EQ(read_value(comp::kFireworkExplosion, vi(0) + vi(3) + be32(1)), Verdict::kMalformed);  // three colours promised

    // tooltip_display: the hidden components are a set that keeps the order they came in
    // (what a vanilla server sent back: tools/vanilla/component_probe.py).
    expect_kept(comp::kTooltipDisplay, Bytes{1} + vi(3) + vi(comp::kLore) + vi(comp::kDamage) + vi(comp::kLore),
                Bytes{1} + vi(2) + vi(comp::kLore) + vi(comp::kDamage));
    EXPECT_EQ(read_value(comp::kTooltipDisplay, Bytes{0} + vi(1) + vi(comp::kCount)), Verdict::kMalformed);
}

TEST(Components, HoldersAreReferencesOnly) {
    namespace reg = proto::reg;
    // ByteBufCodecs.holder: id + 1, or 0 and an inline definition.
    expect_kept(comp::kInstrument, vi(1), vi(1));
    expect_kept(comp::kInstrument, vi(reg::instrument::kCount), vi(reg::instrument::kCount));
    EXPECT_EQ(read_value(comp::kInstrument, vi(reg::instrument::kCount + 1)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kInstrument, vi(0) + Bytes{1, 2, 3}), Verdict::kUnsupported);  // inline: not read yet
    expect_kept(comp::kPaintingVariant, vi(reg::painting_variant::kCount), vi(reg::painting_variant::kCount));
    EXPECT_EQ(read_value(comp::kPaintingVariant, vi(reg::painting_variant::kCount + 1)), Verdict::kMalformed);
    // banner_patterns: layers of (pattern, colour), in order.
    expect_kept(comp::kBannerPatterns, vi(2) + vi(5) + vi(14) + vi(1) + vi(99), vi(2) + vi(5) + vi(14) + vi(1) + vi(0));
    EXPECT_EQ(read_value(comp::kBannerPatterns, vi(1) + vi(reg::banner_pattern::kCount + 1) + vi(0)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kBannerPatterns, vi(1) + vi(0) + str("a:b") + str("x") + vi(0)), Verdict::kUnsupported);
}

TEST(Components, BlockStateIsAMapOfStrings) {
    expect_kept(comp::kBlockState, vi(1) + str("level") + str("7"), vi(1) + str("level") + str("7"));
    // A map: sorted by name here, the last value of a repeated name.
    expect_kept(comp::kBlockState, vi(3) + str("waterlogged") + str("true") + str("level") + str("1") + str("level") + str("9"),
                vi(2) + str("level") + str("9") + str("waterlogged") + str("true"));
    expect_kept(comp::kBlockState, vi(0), vi(0));
    EXPECT_EQ(read_value(comp::kBlockState, vi(1) + str("level")), Verdict::kMalformed);
    Bytes many = vi(17);
    for (int i = 0; i < 17; ++i) many = many + str(std::string(1, static_cast<char>('a' + i))) + str("x");
    EXPECT_EQ(read_value(comp::kBlockState, many), Verdict::kUnsupported);
}

// ---------------------------------------------------------------------------- text and NBT
//
// `n::` builds NBT by hand (nbt_bytes.hpp); the entry order expected of a text component is
// vanilla's (text_test.cpp), that of any other compound is by name.

TEST(Components, StringsAreUtf8OfLimitedLength) {
    const auto potion_named = [](const Bytes& name) { return Bytes{0, 0} + vi(0) + Bytes{1} + name; };
    expect_kept(comp::kPotionContents, potion_named(str("\xD0\x97\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xB5 \xF0\x9F\x98\x80")),
                potion_named(str("\xD0\x97\xD0\xB5\xD0\xBB\xD1\x8C\xD0\xB5 \xF0\x9F\x98\x80")));
    // Bytes Java would decode with a U+FFFD and so write back differently: not kept.
    for (const Bytes& bad : {Bytes{0xFF}, Bytes{0xC3}, Bytes{0xC0, 0x80}, Bytes{0xED, 0xA0, 0x80}, Bytes{0xF4, 0x90, 0x80, 0x80},
                             Bytes{0x80}, Bytes{0xE2, 0x82}})
        EXPECT_EQ(read_value(comp::kPotionContents, potion_named(vi(static_cast<u32>(bad.size())) + bad)), Verdict::kUnsupported);
    EXPECT_EQ(read_value(comp::kBlockState, vi(1) + str("level") + vi(1) + Bytes{0xFF}), Verdict::kUnsupported);
    EXPECT_EQ(read_value(comp::kPotionContents, potion_named(vi(0xFFFFFFFF))), Verdict::kMalformed);  // a negative length
}

TEST(Components, NamesAreTextComponents) {
    const Bytes hi = n::root(8, n::str("hi"));
    for (const u32 type : {comp::kCustomName, comp::kItemName}) {
        expect_kept(type, hi, hi);
        // The spellings of one text are one text, and what is kept is what vanilla writes for it.
        expect_kept(type, n::root(10, n::compound({n::s("text", "hi")})), hi);
        expect_kept(type, n::root(9, n::list(8, {n::str("hi")})), hi);
        expect_kept(type, n::root(10, n::compound({n::b("italic", 0), n::s("text", "Excalibur"), n::s("color", "gold")})),
                    n::root(10, n::compound({n::s("color", "gold"), n::s("text", "Excalibur"), n::b("italic", 0)})));
        expect_kept(type, hi + Bytes{0xAA, 0xBB}, hi);  // lengthPrefixed drops what the codec does not read
        // No tag, half a tag, a tag that is no component.
        EXPECT_EQ(read_value(type, {}), Verdict::kMalformed);
        EXPECT_EQ(read_value(type, {0x00}), Verdict::kMalformed);  // TAG_End
        EXPECT_EQ(read_value(type, {0x08, 0x00, 0x05, 'h', 'i'}), Verdict::kMalformed);
        EXPECT_EQ(read_value(type, n::root(3, n::be32(5))), Verdict::kMalformed);
        EXPECT_EQ(read_value(type, n::root(10, n::compound({n::s("color", "red")}))), Verdict::kMalformed);
        EXPECT_EQ(read_value(type, n::root(8, n::be16(1) + Bytes{0x80})), Verdict::kMalformed);  // not modified UTF-8
        // A component with parts this server does not read, and strings it would not write back as they came.
        EXPECT_EQ(read_value(type, n::root(10, n::compound({n::s("selector", "@a")}))), Verdict::kUnsupported);
        EXPECT_EQ(read_value(type, n::root(8, n::be16(1) + Bytes{0x00})), Verdict::kUnsupported);
    }
    // Nests deeper than vanilla's reader goes.
    Bytes deep = n::root(9, {});
    for (int i = 0; i < 512; ++i) deep = deep + Bytes{9} + n::be32(1);
    EXPECT_EQ(read_value(comp::kCustomName, deep + Bytes{0} + n::be32(0)), Verdict::kMalformed);

    // Two stacks named the same are the same stack, however their clients spelled the name.
    const ItemStack a = with_patch(it::kStone, 1, untrusted_patch({{comp::kCustomName, hi}}));
    const ItemStack b =
        with_patch(it::kStone, 1, untrusted_patch({{comp::kCustomName, n::root(10, n::compound({n::s("text", "hi")}))}}));
    const ItemStack c = with_patch(it::kStone, 1, untrusted_patch({{comp::kCustomName, n::root(8, n::str("ho"))}}));
    EXPECT_NE(a.patch(), 0u);
    EXPECT_TRUE(item::same_item_same_components(a, b));
    EXPECT_FALSE(item::same_item_same_components(a, c));
    EXPECT_FALSE(item::same_item_same_components(a, ItemStack(it::kStone, 1)));
}

TEST(Components, LoreIsAListOfLines) {
    const Bytes one = n::root(8, n::str("one")), two = n::root(10, n::compound({n::s("text", "two"), n::s("color", "gray")}));
    const Bytes two_kept = n::root(10, n::compound({n::s("color", "gray"), n::s("text", "two")}));
    expect_kept(comp::kLore, vi(2) + one + two, vi(2) + one + two_kept);
    expect_kept(comp::kLore, vi(0), vi(0));
    EXPECT_EQ(read_value(comp::kLore, vi(2) + one), Verdict::kMalformed);  // a line short
    EXPECT_EQ(read_value(comp::kLore, vi(1) + n::root(1, {1})), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kLore, vi(2) + one + n::root(10, n::compound({n::s("nbt", "x")}))), Verdict::kUnsupported);
    Bytes many = vi(257);
    for (int i = 0; i < 257; ++i) many = many + n::root(8, n::str(""));
    EXPECT_EQ(read_value(comp::kLore, many), Verdict::kMalformed);  // ItemLore: at most 256
    Bytes most = vi(256);
    for (int i = 0; i < 256; ++i) most = most + n::root(8, n::str(""));
    expect_kept(comp::kLore, most, most);
}

TEST(Components, CustomDataIsACompound) {
    // Kept as vanilla would write the compound after loading it, entries by name.
    const Bytes in =
        n::root(10, n::compound({n::i("uses", 3), n::s("owner", "Notch"), n::c("nested", {n::b("z", 1), n::b("a", 2)}),
                                 n::i("uses", 4), n::l("tags", 10, {n::compound({n::s("", "x")})})}));
    const Bytes kept = n::root(10, n::compound({n::c("nested", {n::b("a", 2), n::b("z", 1)}), n::s("owner", "Notch"),
                                                n::l("tags", 8, {n::str("x")}), n::i("uses", 4)}));
    expect_kept(comp::kCustomData, in, kept);
    expect_kept(comp::kCustomData, kept, kept);
    expect_kept(comp::kCustomData, n::root(10, n::compound({})), n::root(10, n::compound({})));
    expect_kept(comp::kBucketEntityData, in, kept);
    EXPECT_EQ(read_value(comp::kCustomData, n::root(3, n::be32(1))), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kCustomData, n::root(9, n::list(0, {}))), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kCustomData, {0x00}), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kCustomData, {0x0A, 0x01}), Verdict::kMalformed);
    // The Codec of custom_data takes SNBT in a string too; the stream codec of the bucket's data does not.
    EXPECT_EQ(read_value(comp::kCustomData, n::root(8, n::str("{a:1}"))), Verdict::kUnsupported);
    EXPECT_EQ(read_value(comp::kBucketEntityData, n::root(8, n::str("{a:1}"))), Verdict::kMalformed);
    // A tag past vanilla's quota is not kept: these 50 000 empty compounds weigh 2.6 MiB there.
    Bytes heavy = n::root(10, Bytes{9} + n::str("l") + Bytes{10} + n::be32(50000));
    heavy.resize(heavy.size() + 50000, 0);
    EXPECT_EQ(read_value(comp::kCustomData, heavy + Bytes{0}), Verdict::kUnsupported);
    // A long name is a name: 20 000 characters fit a stack (item::kMaxPatchBytes).
    EXPECT_EQ(read_value(comp::kCustomName, n::root(8, n::str(std::string(20000, 'x')))), Verdict::kOk);
    EXPECT_EQ(read_value(comp::kCustomName, n::root(8, n::be16(8000) + Bytes(7999, 'x'))), Verdict::kMalformed);

    // The same data in another order is the same data.
    const ItemStack a = with_patch(it::kStone, 1, untrusted_patch({{comp::kCustomData, in}}));
    const ItemStack b = with_patch(it::kStone, 1, untrusted_patch({{comp::kCustomData, kept}}));
    EXPECT_NE(a.patch(), 0u);
    EXPECT_EQ(a.patch(), b.patch());
}

TEST(Components, EntityDataHasATypeInFront) {
    namespace sreg = proto::gen::static_registry;
    namespace et = proto::gen::entity_type;
    const Bytes data = n::root(10, n::compound({n::s("CustomName", "Bob"), n::b("NoAI", 1)}));
    expect_kept(comp::kEntityData, vi(5) + data, vi(5) + data);
    expect_kept(comp::kEntityData, vi(et::kCount - 1) + data, vi(et::kCount - 1) + data);
    // The registry of entity types has a default: a type that does not exist is a pig, not an error.
    expect_kept(comp::kEntityData, vi(et::kCount) + data, vi(et::kPig) + data);
    expect_kept(comp::kEntityData, vi(0xFFFFFFFF) + data, vi(et::kPig) + data);
    EXPECT_EQ(read_value(comp::kEntityData, vi(5)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kEntityData, vi(5) + n::root(8, n::str("x"))), Verdict::kMalformed);
    // That of block entity types has none.
    expect_kept(comp::kBlockEntityData, vi(sreg::kBlockEntityTypeCount - 1) + data, vi(sreg::kBlockEntityTypeCount - 1) + data);
    EXPECT_EQ(read_value(comp::kBlockEntityData, vi(sreg::kBlockEntityTypeCount) + data), Verdict::kMalformed);
}

TEST(Components, WhatAMobIsIsAnIdOfARegistryOrAnEnum) {
    namespace reg = proto::reg;
    namespace sreg = proto::gen::static_registry;
    // ByteBufCodecs.holderRegistry: an id the registry has.
    expect_kept(comp::kWolfVariant, vi(reg::wolf_variant::kCount - 1), vi(reg::wolf_variant::kCount - 1));
    EXPECT_EQ(read_value(comp::kWolfVariant, vi(reg::wolf_variant::kCount)), Verdict::kMalformed);
    expect_kept(comp::kVillagerVariant, vi(3), vi(3));
    EXPECT_EQ(read_value(comp::kVillagerVariant, vi(sreg::kVillagerTypeCount)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kCatSoundVariant, vi(reg::cat_sound_variant::kCount)), Verdict::kMalformed);
    expect_kept(comp::kProvidesPotteryPattern, vi(0), vi(0));
    // ByteBufCodecs.idMapper: what an id outside the enum becomes is the enum's own choice.
    expect_kept(comp::kFoxVariant, vi(1), vi(1));
    expect_kept(comp::kFoxVariant, vi(7), vi(0));  // ZERO
    expect_kept(comp::kAxolotlVariant, vi(0xFFFFFFFF), vi(0));
    expect_kept(comp::kSalmonSize, vi(9), vi(2));  // CLAMP
    expect_kept(comp::kSalmonSize, vi(0xFFFFFFFF), vi(0));
    expect_kept(comp::kParrotVariant, vi(4), vi(4));
    expect_kept(comp::kLlamaVariant, vi(100), vi(3));
    expect_kept(comp::kMooshroomVariant, vi(2), vi(1));
    expect_kept(comp::kHorseVariant, vi(9), vi(2));  // WRAP: 7 colours
    expect_kept(comp::kHorseVariant, vi(0xFFFFFFFF), vi(6));
    expect_kept(comp::kRabbitVariant, vi(99), vi(99));  // sparse: the killer rabbit is 99
    expect_kept(comp::kRabbitVariant, vi(6), vi(0));
    expect_kept(comp::kTropicalFishPattern, vi(257), vi(257));  // sparse: size | pattern << 8
    expect_kept(comp::kTropicalFishPattern, vi(2), vi(0));
    // The three that only travel.
    expect_kept(comp::kCreativeSlotLock, {}, {});
    expect_kept(comp::kAdditionalTradeCost, vi(0xFFFFFFFB), vi(0xFFFFFFFB));
    expect_kept(comp::kMapPostProcessing, vi(1), vi(1));
    expect_kept(comp::kMapPostProcessing, vi(2), vi(0));
}

TEST(Components, StacksInStacks) {
    const u32 held = item::patches().live();  // (the recipes keep a few stacks with components)
    // ItemStackTemplate.STREAM_CODEC: item, count, and a patch whose values have no length.
    const Bytes stones = vi(it::kStone) + vi(3) + trusted_patch({});
    const Bytes sword =
        vi(it::kDiamondSword) + vi(1) + trusted_patch({{comp::kDamage, vi(7)}, {comp::kMaxStackSize, vi(1)}}, {comp::kLore});
    // ...kept with its entries by type, as a patch of a stack is.
    const Bytes sword_kept =
        vi(it::kDiamondSword) + vi(1) + trusted_patch({{comp::kMaxStackSize, vi(1)}, {comp::kDamage, vi(7)}}, {comp::kLore});
    ASSERT_LT(comp::kMaxStackSize, comp::kDamage);
    // container: at most 256 slots, each a flag and then a template.
    expect_kept(comp::kContainer, vi(0), vi(0));
    expect_kept(comp::kContainer, vi(3) + Bytes{1} + stones + Bytes{0} + Bytes{1} + sword + Bytes{0x55},
                vi(3) + Bytes{1} + stones + Bytes{0} + Bytes{1} + sword_kept);
    expect_kept(comp::kContainer, vi(2) + Bytes{1} + stones + Bytes{0},
                vi(2) + Bytes{1} + stones + Bytes{0});  // an empty slot at the end stays
    {
        Bytes many = vi(257);
        for (u32 i = 0; i < 257; ++i) many = many + Bytes{0};
        EXPECT_EQ(read_value(comp::kContainer, many), Verdict::kMalformed);
    }
    EXPECT_EQ(read_value(comp::kContainer, vi(1) + Bytes{1} + vi(it::kStone)), Verdict::kMalformed);  // ends in the stack
    // The template refuses air and a count of 0; and no count but 1..99 can be written into a tag,
    // which is tried with what a client sends.
    EXPECT_EQ(read_value(comp::kSulfurCubeContent, vi(it::kAir) + vi(1) + trusted_patch({})), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kSulfurCubeContent, vi(it::kCount) + vi(1) + trusted_patch({})), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kSulfurCubeContent, vi(it::kStone) + vi(0) + trusted_patch({})), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kSulfurCubeContent, vi(it::kStone) + vi(100) + trusted_patch({})), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kSulfurCubeContent, vi(it::kStone) + vi(0xFFFFFFFF) + trusted_patch({})), Verdict::kMalformed);
    expect_kept(comp::kSulfurCubeContent, stones, stones);
    // What is in the stack inside is read as any component is.
    EXPECT_EQ(
        read_value(comp::kBundleContents, vi(1) + vi(it::kStone) + vi(1) + trusted_patch({{comp::kDamage, vi(0xFFFFFFFF)}})),
        Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kBundleContents, vi(1) + vi(it::kStone) + vi(1) + trusted_patch({{comp::kTool, vi(0)}})),
              Verdict::kUnsupported);
    EXPECT_EQ(read_value(comp::kBundleContents, vi(1) + vi(it::kStone) + vi(1) + trusted_patch({{comp::kCount, {}}})),
              Verdict::kMalformed);
    expect_kept(comp::kBundleContents, vi(2) + sword + stones, vi(2) + sword_kept + stones);
    // charged_projectiles: at most 1024.
    expect_kept(comp::kChargedProjectiles, vi(1) + stones, vi(1) + stones);
    {
        Bytes many = vi(1025);
        for (u32 i = 0; i < 1025; ++i) many = many + stones;
        EXPECT_EQ(read_value(comp::kChargedProjectiles, many), Verdict::kMalformed);
    }
    // A stack in a stack in a stack: a shulker box in a bundle in a bundle.
    const Bytes box = vi(it::kShulkerBox) + vi(1) + trusted_patch({{comp::kContainer, vi(1) + Bytes{1} + sword}});
    const Bytes box_kept = vi(it::kShulkerBox) + vi(1) + trusted_patch({{comp::kContainer, vi(1) + Bytes{1} + sword_kept}});
    const Bytes bundle = vi(it::kBundle) + vi(1) + trusted_patch({{comp::kBundleContents, vi(1) + box}});
    const Bytes bundle_kept = vi(it::kBundle) + vi(1) + trusted_patch({{comp::kBundleContents, vi(1) + box_kept}});
    expect_kept(comp::kBundleContents, vi(1) + bundle, vi(1) + bundle_kept);
    {
        Bytes deep = vi(1) + stones;  // ...and one that goes deeper than any real one: not followed
        for (u32 i = 0; i < 12; ++i) deep = vi(1) + vi(it::kBundle) + vi(1) + trusted_patch({{comp::kBundleContents, deep}});
        EXPECT_EQ(read_value(comp::kBundleContents, deep), Verdict::kUnsupported);
    }
    // pot_decorations: back, left, right, front.
    expect_kept(comp::kPotDecorations, Bytes{1} + stones + Bytes{0, 0} + Bytes{1} + sword,
                Bytes{1} + stones + Bytes{0, 0} + Bytes{1} + sword_kept);
    EXPECT_EQ(read_value(comp::kPotDecorations, Bytes{0, 0, 0}), Verdict::kMalformed);
    // bees: the entity as typed data, how long it has been in, how long it must be.
    namespace et = proto::gen::entity_type;
    const Bytes bee = vi(et::kBee) + n::root(10, n::compound({n::b("NoAI", 1)})) + vi(5) + vi(2400);
    expect_kept(comp::kBees, vi(2) + bee + bee, vi(2) + bee + bee);
    EXPECT_EQ(read_value(comp::kBees, vi(1) + vi(et::kBee) + n::root(8, n::str("x")) + vi(5) + vi(5)), Verdict::kMalformed);
    EXPECT_EQ(item::patches().live(), held);
}

TEST(Components, TheRestOfWhatIsOnlyData) {
    const u32 held = item::patches().live();  // (the recipes keep a few stacks with components)
    namespace reg = proto::reg;
    namespace sreg = proto::gen::static_registry;
    // custom_model_data: floats, flags, strings, colours.
    const Bytes model = vi(2) + f32(1.5f) + f32(-0.0f) + vi(2) + Bytes{1, 0} + vi(1) + str("a") + vi(1) + be32(0x00FF8800);
    expect_kept(comp::kCustomModelData, model, model);
    expect_kept(comp::kCustomModelData, vi(0) + vi(1) + Bytes{7} + vi(0) + vi(0), vi(0) + vi(1) + Bytes{1} + vi(0) + vi(0));
    EXPECT_EQ(read_value(comp::kCustomModelData, vi(2) + f32(1.0f)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kCustomModelData, vi(0) + vi(0) + vi(0)), Verdict::kMalformed);
    // trim: material and pattern, each id + 1 - or 0 and the entry itself, which is not read.
    expect_kept(comp::kTrim, vi(1) + vi(reg::trim_pattern::kCount), vi(1) + vi(reg::trim_pattern::kCount));
    EXPECT_EQ(read_value(comp::kTrim, vi(0)), Verdict::kUnsupported);
    EXPECT_EQ(read_value(comp::kTrim, vi(reg::trim_material::kCount + 1) + vi(1)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kTrim, vi(1) + vi(reg::trim_pattern::kCount + 1)), Verdict::kMalformed);
    expect_kept(comp::kProvidesTrimMaterial, vi(2), vi(2));
    // provides_banner_patterns: a HolderSet - 0 and a tag the registry has, or n + 1 and n ids.
    expect_kept(comp::kProvidesBannerPatterns, vi(0) + str("pattern_item/globe"), vi(0) + str("minecraft:pattern_item/globe"));
    EXPECT_EQ(read_value(comp::kProvidesBannerPatterns, vi(0) + str("minecraft:no_such_tag")), Verdict::kMalformed);
    expect_kept(comp::kProvidesBannerPatterns, vi(3) + vi(4) + vi(0), vi(3) + vi(4) + vi(0));
    expect_kept(comp::kProvidesBannerPatterns, vi(1), vi(1));
    EXPECT_EQ(read_value(comp::kProvidesBannerPatterns, vi(2) + vi(reg::banner_pattern::kCount)), Verdict::kMalformed);
    // lodestone_tracker: perhaps a dimension and a position, and whether it is tracked.
    const Bytes pos = be32(0x12345678) + be32(0x9ABCDEF0);
    expect_kept(comp::kLodestoneTracker, Bytes{1} + str("the_nether") + pos + Bytes{1},
                Bytes{1} + str("minecraft:the_nether") + pos + Bytes{1});
    expect_kept(comp::kLodestoneTracker, Bytes{0, 0}, Bytes{0, 0});
    EXPECT_EQ(read_value(comp::kLodestoneTracker, Bytes{1} + str("Bad Name") + pos + Bytes{1}), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kLodestoneTracker, Bytes{1} + str("a") + be32(1)), Verdict::kMalformed);
    // break_sound: id + 1, or 0 and the sound itself - its identifier, perhaps a range.
    expect_kept(comp::kBreakSound, vi(sreg::kSoundEventCount), vi(sreg::kSoundEventCount));
    EXPECT_EQ(read_value(comp::kBreakSound, vi(sreg::kSoundEventCount + 1)), Verdict::kMalformed);
    expect_kept(comp::kBreakSound, vi(0) + str("my:sound") + Bytes{1} + f32(16.0f),
                vi(0) + str("my:sound") + Bytes{1} + f32(16.0f));
    expect_kept(comp::kBreakSound, vi(0) + str("sound") + Bytes{0}, vi(0) + str("minecraft:sound") + Bytes{0});
    // profile: a whole game profile or a part of one, and a skin patch.
    const Bytes uuid = be32(1) + be32(2) + be32(3) + be32(4);
    const Bytes no_skin = Bytes{0, 0, 0, 0};
    const Bytes textures = str("textures") + str("abc") + Bytes{1} + str("sig"), other = str("other") + str("v") + Bytes{0},
                again = str("textures") + str("def") + Bytes{0};
    expect_kept(comp::kProfile, Bytes{1} + uuid + str("Notch") + vi(0) + no_skin,
                Bytes{1} + uuid + str("Notch") + vi(0) + no_skin);
    // (its properties are a multimap: those of one name stand together)
    expect_kept(comp::kProfile, Bytes{1} + uuid + str("Notch") + vi(3) + textures + other + again + no_skin,
                Bytes{1} + uuid + str("Notch") + vi(3) + textures + again + other + no_skin);
    const Bytes skin = Bytes{1} + str("my:skin") + Bytes{0} + Bytes{1} + str("wings") + Bytes{1, 7};
    expect_kept(comp::kProfile, Bytes{0} + Bytes{1} + str("Steve") + Bytes{0} + vi(0) + skin,
                Bytes{0} + Bytes{1} + str("Steve") + Bytes{0} + vi(0) + Bytes{1} + str("my:skin") + Bytes{0} + Bytes{1} +
                    str("minecraft:wings") + Bytes{1, 1});
    expect_kept(comp::kProfile, Bytes{0, 0} + Bytes{1} + uuid + vi(0) + no_skin, Bytes{0, 0} + Bytes{1} + uuid + vi(0) + no_skin);
    expect_kept(comp::kProfile, Bytes{0, 0, 0} + vi(0) + no_skin, Bytes{0, 0, 0} + vi(0) + no_skin);
    EXPECT_EQ(read_value(comp::kProfile, Bytes{1} + uuid + str("SeventeenLettersXY") + vi(0) + no_skin), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kProfile, Bytes{1} + uuid + str("Notch") + vi(17)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kProfile, Bytes{1} + uuid + str("Notch") + vi(0) + Bytes{0, 0}), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kProfile, Bytes{1} + be32(1)), Verdict::kMalformed);
    // lock: no StreamCodec - the tag of an ItemPredicate.
    expect_kept(comp::kLock, n::root(10, n::compound({n::s("items", "stone"), n::c("count", {n::i("min", 2), n::i("max", 2)})})),
                n::root(10, n::compound({n::i("count", 2), n::s("items", "minecraft:stone")})));
    EXPECT_EQ(read_value(comp::kLock, n::root(10, n::compound({n::s("items", "no_such_item")}))), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kLock, n::root(10, n::compound({n::c("predicates", {n::c("damage", {})})}))),
              Verdict::kUnsupported);
    // What has no StreamCodec travels as the tag its Codec writes - and is kept as that Codec
    // writes it again, the entries of a compound in the order of vanilla's HashMap.
    expect_kept(comp::kIntangibleProjectile, n::root(10, n::compound({n::b("x", 1)})), n::root(10, n::compound({})));
    EXPECT_EQ(read_value(comp::kIntangibleProjectile, n::root(1, Bytes{1})), Verdict::kMalformed);
    expect_kept(comp::kRecipes, n::root(9, n::list(8, {n::str("stick"), n::str("my:recipe")})),
                n::root(9, n::list(8, {n::str("minecraft:stick"), n::str("my:recipe")})));
    EXPECT_EQ(read_value(comp::kRecipes, n::root(9, n::list(8, {n::str("Not An Id")}))), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kRecipes, n::root(8, n::str("stick"))), Verdict::kMalformed);
    expect_kept(comp::kContainerLoot,
                n::root(10, n::compound({n::s("loot_table", "chests/simple_dungeon"), n::entry(4, "seed", n::be64(0))})),
                n::root(10, n::compound({n::s("loot_table", "minecraft:chests/simple_dungeon")})));
    EXPECT_EQ(read_value(comp::kContainerLoot, n::root(10, n::compound({n::entry(4, "seed", n::be64(5))}))), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kDebugStickState, n::root(10, n::compound({n::s("minecraft:oak_stairs", "nope")}))),
              Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kDebugStickState, n::root(10, n::compound({n::s("minecraft:no_block", "facing")}))),
              Verdict::kMalformed);
    expect_kept(comp::kDebugStickState, n::root(10, n::compound({n::s("oak_stairs", "facing")})),
                n::root(10, n::compound({n::s("minecraft:oak_stairs", "facing")})));
    EXPECT_EQ(read_value(comp::kMapDecorations, n::root(10, n::compound({n::s("a", "b")}))), Verdict::kMalformed);
    EXPECT_EQ(item::patches().live(), held);
}

TEST(Components, Books) {
    // writable_book_content: pages of text, each perhaps with what a chat filter made of it.
    expect_kept(comp::kWritableBookContent, vi(2) + str("page one") + Bytes{0} + str("page two") + Bytes{1} + str("p*** two"),
                vi(2) + str("page one") + Bytes{0} + str("page two") + Bytes{1} + str("p*** two"));
    expect_kept(comp::kWritableBookContent, vi(0), vi(0));
    expect_kept(comp::kWritableBookContent, vi(1) + str(std::string(900, 'x')) + Bytes{0},
                vi(1) + str(std::string(900, 'x')) + Bytes{0});
    // A page may have 1024 characters; a book of a hundred such pages is a stack like any other.
    expect_kept(comp::kWritableBookContent, vi(1) + str(std::string(1024, 'x')) + Bytes{0},
                vi(1) + str(std::string(1024, 'x')) + Bytes{0});
    {
        Bytes book = vi(100);
        for (int i = 0; i < 100; ++i) book = book + str(std::string(1024, static_cast<char>('a' + i % 26))) + Bytes{0};
        expect_kept(comp::kWritableBookContent, book, book);
    }
    EXPECT_EQ(read_value(comp::kWritableBookContent, vi(1) + str(std::string(1025, 'x')) + Bytes{0}), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kWritableBookContent, vi(1) + str("page")), Verdict::kMalformed);  // the filtered flag is missing
    Bytes pages = vi(101);
    for (int i = 0; i < 101; ++i) pages = pages + str("") + Bytes{0};
    EXPECT_EQ(read_value(comp::kWritableBookContent, pages), Verdict::kMalformed);  // at most 100

    // written_book_content: title, author, generation, pages of components, resolved.
    const Bytes page = n::root(10, n::compound({n::s("text", "Once"), n::b("bold", 1)}));
    const Bytes book = str("Title") + Bytes{1} + str("T****") + str("Author") + vi(2) + vi(2) + page + Bytes{0} +
                       n::root(8, n::str("The end")) + Bytes{1} + n::root(8, n::str("The ***")) + Bytes{7};
    const Bytes book_kept = str("Title") + Bytes{1} + str("T****") + str("Author") + vi(2) + vi(2) + page + Bytes{0} +
                            n::root(8, n::str("The end")) + Bytes{1} + n::root(8, n::str("The ***")) + Bytes{1};
    expect_kept(comp::kWrittenBookContent, book, book_kept);
    const auto titled = [](const Bytes& title, u32 generation) {
        return title + Bytes{0} + str("A") + vi(generation) + vi(0) + Bytes{0};
    };
    expect_kept(comp::kWrittenBookContent, titled(str(std::string(32, 't')), 3), titled(str(std::string(32, 't')), 3));
    EXPECT_EQ(read_value(comp::kWrittenBookContent, titled(str(std::string(33, 't')), 0)), Verdict::kMalformed);
    // 32 characters, not bytes: sixteen of U+1F600 are 32 UTF-16 units in 64 bytes.
    std::string faces;
    for (int i = 0; i < 16; ++i) faces += "\xF0\x9F\x98\x80";
    expect_kept(comp::kWrittenBookContent, titled(str(faces), 0), titled(str(faces), 0));
    EXPECT_EQ(read_value(comp::kWrittenBookContent, titled(str(faces + "x"), 0)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kWrittenBookContent, titled(str("t"), 4)), Verdict::kMalformed);  // generation: 0 .. 3
    EXPECT_EQ(read_value(comp::kWrittenBookContent, titled(str("t"), 0xFFFFFFFF)), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kWrittenBookContent,
                         str("t") + Bytes{0} + str("A") + vi(0) + vi(1) + n::root(3, n::be32(1)) + Bytes{0, 0}),
              Verdict::kMalformed);  // a page that is no component
}

TEST(Components, SignText) {
    const Bytes a = n::root(8, n::str("a")), b = n::root(8, n::str("b")), empty = n::root(8, n::str(""));
    const Bytes lines = a + b + empty + empty;
    // Four lines without a count, no filtered lines, the dye, the glow.
    expect_kept(comp::kSignTextFront, lines + Bytes{0} + vi(4) + Bytes{1}, lines + Bytes{0} + vi(4) + Bytes{1});
    expect_kept(comp::kSignTextBack, lines + Bytes{0} + vi(99) + Bytes{9}, lines + Bytes{0} + vi(0) + Bytes{1});
    // Filtered lines that say the same - however spelled - are not sent on.
    const Bytes same = n::root(10, n::compound({n::s("text", "a")})) + b + empty + empty;
    expect_kept(comp::kSignTextFront, lines + Bytes{1} + same + vi(0) + Bytes{0}, lines + Bytes{0} + vi(0) + Bytes{0});
    const Bytes other = a + n::root(8, n::str("*")) + empty + empty;
    expect_kept(comp::kSignTextFront, lines + Bytes{1} + other + vi(0) + Bytes{0}, lines + Bytes{1} + other + vi(0) + Bytes{0});
    EXPECT_EQ(read_value(comp::kSignTextFront, a + b + empty + Bytes{0} + vi(0) + Bytes{0}), Verdict::kMalformed);  // three lines
    EXPECT_EQ(read_value(comp::kSignTextFront, lines + Bytes{1} + a + vi(0) + Bytes{0}), Verdict::kMalformed);
    EXPECT_EQ(read_value(comp::kSignTextFront, lines + Bytes{0} + vi(0)), Verdict::kMalformed);
}

// ---------------------------------------------------------------------------- what a patch changes

TEST(ItemStack, PatchOverridesTheItemsDefaults) {
    const ItemStack big = with_patch(it::kEnderPearl, 1, untrusted_patch({{comp::kMaxStackSize, vi(99)}}));
    EXPECT_EQ(big.max_stack(), 99u);
    const ItemStack single = with_patch(it::kStone, 1, untrusted_patch({}, {comp::kMaxStackSize}));
    EXPECT_EQ(single.max_stack(), 1u);  // getOrDefault(MAX_STACK_SIZE, 1)
    EXPECT_FALSE(single.stackable());

    const ItemStack worn = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(10)}}));
    EXPECT_TRUE(worn.damageable() && worn.damaged());
    const ItemStack fresh = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(0)}}));
    EXPECT_FALSE(fresh.damaged());
    const ItemStack forever =
        with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(10)}, {comp::kUnbreakable, {}}}));
    EXPECT_FALSE(forever.damageable() || forever.damaged());
    const ItemStack no_bar = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(10)}}, {comp::kMaxDamage}));
    EXPECT_FALSE(no_bar.damageable());
    // Damage on something that has no max damage means nothing.
    EXPECT_FALSE(with_patch(it::kStone, 1, untrusted_patch({{comp::kDamage, vi(10)}})).damaged());
    EXPECT_TRUE(with_patch(it::kStone, 1,
                           untrusted_patch({{comp::kDamage, vi(10)}, {comp::kMaxDamage, vi(50)}, {comp::kMaxStackSize, vi(1)}}))
                    .damaged());

    namespace equip = item::equip;
    EXPECT_EQ(with_patch(it::kDiamondHelmet, 1, untrusted_patch({{comp::kDamage, vi(1)}})).equip_slot(), equip::kHead);
    EXPECT_EQ(with_patch(it::kDiamondHelmet, 1, untrusted_patch({}, {comp::kEquippable})).equip_slot(), item::kNoEquipSlot);

    // A spear whose attack animation was taken away swings like anything else.
    const ItemStack blunt = with_patch(it::kWoodenSpear, 1, untrusted_patch({}, {comp::kAttackAnimation}));
    EXPECT_EQ(blunt.attack_swing().type, proto::swing_animation::kWhack);
    EXPECT_EQ(blunt.attack_swing().ticks, 6u);
    EXPECT_EQ(with_patch(it::kWoodenSpear, 1, untrusted_patch({{comp::kDamage, vi(1)}})).attack_swing().ticks, 13u);
}

// ---------------------------------------------------------------------------- whole stacks on the wire

TEST(ItemWire, UntrustedStack) {
    const auto read = [](const Bytes& in, ItemStack& s, bool* done = nullptr) {
        Reader r(in.data(), in.size());
        const Verdict v = item::read_untrusted(r, s);
        if (done) *done = r.done();
        return v;
    };
    ItemStack s(it::kDirt, 1);
    bool done = false;
    EXPECT_EQ(read(vi(0), s, &done), Verdict::kOk);  // an empty stack is one byte
    EXPECT_TRUE(s.empty() && done);
    EXPECT_EQ(read(vi(0xFFFFFFFF), s, &done), Verdict::kOk);  // so is a negative count
    EXPECT_TRUE(s.empty() && done);
    EXPECT_EQ(read(vi(64) + vi(it::kStone) + vi(0) + vi(0), s, &done), Verdict::kOk);
    EXPECT_TRUE(done);
    EXPECT_TRUE(item::matches(s, ItemStack(it::kStone, 64)));
    EXPECT_EQ(read(vi(99) + vi(it::kStone) + vi(0) + vi(0), s),
              Verdict::kOk);  // more than a stack of stone: the handler's business
    EXPECT_EQ(s.count(), 99u);
    EXPECT_EQ(read(vi(100) + vi(it::kStone) + vi(0) + vi(0), s), Verdict::kMalformed);  // ItemStack.CODEC: 1..99
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(read(vi(1) + vi(it::kCount) + vi(0) + vi(0), s), Verdict::kMalformed);  // no such item
    EXPECT_EQ(read(vi(1) + vi(it::kStone), s), Verdict::kMalformed);                  // the patch is missing
    EXPECT_EQ(read(vi(5) + vi(it::kAir) + vi(0) + vi(0), s, &done), Verdict::kOk);    // air is nothing
    EXPECT_TRUE(s.empty() && done);
    const u32 live = item::patches().live();
    EXPECT_EQ(read(vi(200) + vi(it::kAir) + untrusted_patch({{comp::kDamage, vi(1)}}), s, &done), Verdict::kOk);
    EXPECT_TRUE(s.empty() && done);
    EXPECT_EQ(read(vi(100) + vi(it::kStone) + untrusted_patch({{comp::kDamage, vi(1)}}), s), Verdict::kMalformed);
    EXPECT_EQ(item::patches().live(), live);  // neither kept its patch
    EXPECT_EQ(read(vi(1) + vi(it::kStone) + untrusted_patch({{comp::kTool, vi(0)}}) + Bytes{0x77}, s, &done),
              Verdict::kUnsupported);
    EXPECT_TRUE(s.empty());
    EXPECT_FALSE(done);  // the reader stands behind the stack, at the 0x77
}

TEST(ItemWire, TrustedStack) {
    u8 buf[256];
    const auto put = [&](const ItemStack& s) {
        const ItemView v = item::view(s);
        const u8* end = put_item(buf, v);
        EXPECT_LE(static_cast<usize>(end - buf), item_bound(v));
        return Bytes(buf, buf + (end - buf));
    };
    const Bytes no_patch{0, 0};  // DataComponentPatch.EMPTY: no added entries, no removed ones
    EXPECT_EQ(put(ItemStack()), Bytes{0});
    EXPECT_EQ(put(ItemStack(it::kStone, 0)), Bytes{0});
    EXPECT_EQ(put(ItemStack(it::kStone, 64)), vi(64) + vi(it::kStone) + no_patch);
    EXPECT_EQ(put(ItemStack(it::kDiamondSword, 1)), vi(1) + vi(it::kDiamondSword) + no_patch);  // a two-byte item id
    const ItemStack worn = with_patch(it::kDiamondSword, 1, untrusted_patch({{comp::kDamage, vi(300)}}, {comp::kLore}));
    EXPECT_EQ(put(worn), vi(1) + vi(it::kDiamondSword) + trusted_patch({{comp::kDamage, vi(300)}}, {comp::kLore}));
    EXPECT_EQ(item_bound(item::view(ItemStack(it::kStone, 64))), 4u);  // tight: it decides the frame header
}

// ---------------------------------------------------------------------------- components as tags
//
// The Codec side (item/codec.hpp). A value is given as the SNBT a command would carry - read by
// the SNBT reader, which has tests of its own - and compared with what comes back out of the
// store: Codec in, StreamCodec kept, Codec out. What a vanilla server writes for the same
// values is compared by tools/vanilla/component_probe.py (`data get ... SelectedItem`).

namespace {

// The component as its Codec writes it after reading `snbt`; "malformed" / "unsupported".
std::string through(u32 type, std::string_view snbt) {
    base::ByteBuf tag, wire, back;
    nbt::SnbtError why;
    EXPECT_TRUE(nbt::read_snbt_fully(snbt, tag, why)) << snbt;
    nbt::Doc doc;
    EXPECT_EQ(doc.parse({tag.data, tag.len}, nbt::Form::kNameless, nbt::kTrusted), nbt::Error::kNone);
    const Verdict v = item::value_from_nbt(type, doc.root(), wire);
    if (v != Verdict::kOk) return v == Verdict::kMalformed ? "malformed" : "unsupported";
    nbt::Writer w(back);
    w.root(nbt::kCompound);
    item::value_to_nbt(type, {wire.data, wire.len}, w, "v");
    w.end_compound();
    nbt::Doc out;
    EXPECT_EQ(out.parse({back.data, back.len}, nbt::Form::kNameless, nbt::kTrusted), nbt::Error::kNone);
    std::string text;
    nbt::print_snbt(out.root().get("v"), text);
    return text;
}

}  // namespace

TEST(ComponentCodec, NumbersNamesAndUnits) {
    EXPECT_EQ(through(comp::kDamage, "3"), "3");
    EXPECT_EQ(through(comp::kDamage, "3b"), "3");    // Codec.INT takes any number
    EXPECT_EQ(through(comp::kDamage, "3.9f"), "3");  // ...as Number.intValue()
    EXPECT_EQ(through(comp::kDamage, "-1"), "malformed");
    EXPECT_EQ(through(comp::kDamage, "\"3\""), "malformed");
    EXPECT_EQ(through(comp::kMaxStackSize, "99"), "99");
    EXPECT_EQ(through(comp::kMaxStackSize, "100"), "malformed");
    EXPECT_EQ(through(comp::kUnbreakable, "{}"), "{}");
    EXPECT_EQ(through(comp::kUnbreakable, "{a:1}"), "{}");
    EXPECT_EQ(through(comp::kUnbreakable, "1b"), "malformed");
    EXPECT_EQ(through(comp::kEnchantmentGlintOverride, "true"), "1b");
    EXPECT_EQ(through(comp::kEnchantmentGlintOverride, "256"), "0b");  // Number.byteValue()
    EXPECT_EQ(through(comp::kMinimumAttackCharge, "0.5"), "0.5f");
    EXPECT_EQ(through(comp::kMinimumAttackCharge, "1.5f"), "malformed");
    EXPECT_EQ(through(comp::kRarity, "epic"), "\"epic\"");
    EXPECT_EQ(through(comp::kRarity, "\"legendary\""), "malformed");
    EXPECT_EQ(through(comp::kBaseColor, "light_blue"), "\"light_blue\"");
    EXPECT_EQ(through(comp::kItemModel, "\"stick\""), "\"minecraft:stick\"");
    EXPECT_EQ(through(comp::kItemModel, "\"Not An Id\""), "malformed");
    EXPECT_EQ(through(comp::kDyedColor, "16711680"), "16711680");
    EXPECT_EQ(through(comp::kDyedColor, "[1.0f,0.5f,0.0f]"), "-33024");  // ARGB.colorFromFloat(1, r, g, b)
    EXPECT_EQ(through(comp::kFood, "{}"), "unsupported");
}

TEST(ComponentCodec, TextAndTags) {
    EXPECT_EQ(through(comp::kCustomName, "\"hi\""), "\"hi\"");
    EXPECT_EQ(through(comp::kCustomName, "{text:\"hi\",color:\"red\",unknown:1}"), "{color:\"red\",text:\"hi\"}");
    EXPECT_EQ(through(comp::kCustomName, "5"), "malformed");
    EXPECT_EQ(through(comp::kLore, "[\"a\",{text:\"b\",bold:true}]"), "[\"a\",{bold:1b,text:\"b\"}]");
    EXPECT_EQ(through(comp::kLore, "[]"), "[]");
    EXPECT_EQ(through(comp::kCustomData, "{b:1,a:{c:2}}"), "{a:{c:2},b:1}");
    EXPECT_EQ(through(comp::kCustomData, "\"{a:1b}\""), "{a:1b}");  // its SNBT in a string
    EXPECT_EQ(through(comp::kCustomData, "5"), "malformed");
    EXPECT_EQ(through(comp::kEntityData, "{id:\"pig\",Health:5f}"), "{Health:5.0f,id:\"minecraft:pig\"}");
    EXPECT_EQ(through(comp::kEntityData, "{Health:5f}"), "malformed");  // no type
    EXPECT_EQ(through(comp::kBlockEntityData, "{id:\"minecraft:chest\",Items:[]}"), "{Items:[],id:\"minecraft:chest\"}");
    EXPECT_EQ(through(comp::kBlockEntityData, "{id:\"nothing\"}"), "malformed");
}

TEST(ComponentCodec, Records) {
    EXPECT_EQ(through(comp::kEnchantments, "{sharpness:5,\"minecraft:unbreaking\":3}"),
              "{\"minecraft:sharpness\":5,\"minecraft:unbreaking\":3}");
    EXPECT_EQ(through(comp::kEnchantments, "{sharpness:0}"), "malformed");
    EXPECT_EQ(through(comp::kEnchantments, "{nothing:1}"), "malformed");
    EXPECT_EQ(through(comp::kTooltipDisplay, "{}"), "{}");
    EXPECT_EQ(through(comp::kTooltipDisplay, "{hide_tooltip:1b,hidden_components:[\"damage\",\"minecraft:lore\"]}"),
              "{hidden_components:[\"minecraft:damage\",\"minecraft:lore\"],hide_tooltip:1b}");
    EXPECT_EQ(through(comp::kPotionContents, "\"swiftness\""), "{potion:\"minecraft:swiftness\"}");
    EXPECT_EQ(
        through(
            comp::kPotionContents,
            "{custom_color:255,custom_effects:[{id:\"speed\",amplifier:2b,duration:100,show_particles:0b}],custom_name:\"x\"}"),
        "{custom_color:255,custom_effects:[{amplifier:2b,duration:100,id:\"minecraft:speed\",show_icon:0b,show_particles:0b}],"
        "custom_name:\"x\"}");
    EXPECT_EQ(through(comp::kSuspiciousStewEffects, "[{id:\"blindness\"},{id:\"speed\",duration:5}]"),
              "[{id:\"minecraft:blindness\"},{duration:5,id:\"minecraft:speed\"}]");
    EXPECT_EQ(through(comp::kFireworkExplosion, "{shape:\"star\",colors:[I;1,2],has_trail:1b}"),
              "{colors:[1,2],has_trail:1b,shape:\"star\"}");
    EXPECT_EQ(through(comp::kFireworkExplosion, "{shape:\"cube\"}"), "malformed");
    EXPECT_EQ(through(comp::kFireworks, "{flight_duration:2b,explosions:[{shape:\"burst\",colors:[5]}]}"),
              "{explosions:[{colors:[5],shape:\"burst\"}],flight_duration:2b}");
    EXPECT_EQ(through(comp::kBannerPatterns, "[{pattern:\"creeper\",color:\"red\"}]"),
              "[{color:\"red\",pattern:\"minecraft:creeper\"}]");
    EXPECT_EQ(through(comp::kBlockState, "{lit:\"true\",facing:\"north\"}"), "{facing:\"north\",lit:\"true\"}");
    EXPECT_EQ(through(comp::kBlockState, "{lit:1b}"), "malformed");
    EXPECT_EQ(through(comp::kWritableBookContent, "{pages:[\"one\",{raw:\"two\",filtered:\"2\"}]}"),
              "{pages:[{raw:\"one\"},{filtered:\"2\",raw:\"two\"}]}");
    EXPECT_EQ(through(comp::kWritableBookContent, "{}"), "{}");
    EXPECT_EQ(through(comp::kWrittenBookContent, "{title:\"T\",author:\"A\",pages:[\"p\",{text:\"q\",bold:1b}],resolved:1b}"),
              "{author:\"A\",pages:[{raw:\"p\"},{raw:{bold:1b,text:\"q\"}}],resolved:1b,title:{raw:\"T\"}}");
    EXPECT_EQ(through(comp::kWrittenBookContent, "{title:\"T\"}"), "malformed");  // no author
    EXPECT_EQ(through(comp::kSignTextFront, "{messages:[\"a\",\"b\",\"\",\"\"],color:\"red\"}"),
              "{color:\"red\",has_glowing_text:0b,messages:[\"a\",\"b\",\"\",\"\"]}");
    EXPECT_EQ(through(comp::kSignTextFront, "{messages:[\"a\"]}"), "malformed");
    EXPECT_EQ(through(comp::kInstrument, "\"ponder_goat_horn\""), "\"minecraft:ponder_goat_horn\"");
    EXPECT_EQ(through(comp::kInstrument, "{sound_event:\"x\"}"), "unsupported");  // an instrument of its own
}

TEST(ComponentCodec, StacksAndPatches) {
    const u32 held = item::patches().live();  // (the recipes keep a few stacks with components)
    const auto stack = [](std::string_view snbt) -> std::string {
        base::ByteBuf tag, back;
        nbt::SnbtError why;
        EXPECT_TRUE(nbt::read_snbt_fully(snbt, tag, why)) << snbt;
        nbt::Doc doc;
        EXPECT_EQ(doc.parse({tag.data, tag.len}, nbt::Form::kNameless, nbt::kTrusted), nbt::Error::kNone);
        ItemStack s;
        const Verdict v = item::stack_from_nbt(doc.root(), s);
        if (v != Verdict::kOk) return v == Verdict::kMalformed ? "malformed" : "unsupported";
        nbt::Writer w(back);
        w.root(nbt::kCompound);
        item::stack_to_nbt(s, w);
        w.end_compound();
        nbt::Doc out;
        EXPECT_EQ(out.parse({back.data, back.len}, nbt::Form::kNameless, nbt::kTrusted), nbt::Error::kNone);
        std::string text;
        nbt::print_snbt(out.root(), text);
        return text;
    };
    EXPECT_EQ(stack("{id:\"stone\"}"), "{count:1,id:\"minecraft:stone\"}");
    EXPECT_EQ(stack("{id:\"minecraft:stone\",count:64}"), "{count:64,id:\"minecraft:stone\"}");
    EXPECT_EQ(stack("{id:\"stone\",count:5,components:{damage:3,\"!minecraft:lore\":{},\"minecraft:unbreakable\":{}}}"),
              "{components:{\"!minecraft:lore\":{},\"minecraft:damage\":3,\"minecraft:unbreakable\":{}},count:5,id:\"minecraft:"
              "stone\"}");
    EXPECT_EQ(stack("{id:\"stone\",components:{}}"), "{count:1,id:\"minecraft:stone\"}");
    EXPECT_EQ(stack("{id:\"air\"}"), "malformed");
    EXPECT_EQ(stack("{id:\"nothing\"}"), "malformed");
    EXPECT_EQ(stack("{id:\"stone\",count:0}"), "malformed");
    EXPECT_EQ(stack("{id:\"stone\",count:100}"), "malformed");
    EXPECT_EQ(stack("{count:1}"), "malformed");
    EXPECT_EQ(stack("{id:\"stone\",components:{nothing:1}}"), "malformed");
    EXPECT_EQ(stack("{id:\"stone\",components:{damage:-1}}"), "malformed");
    EXPECT_EQ(stack("{id:\"stone\",components:{food:{}}}"), "unsupported");
    EXPECT_EQ(item::patches().live(), held);

    // The names of components: with the namespace or without.
    EXPECT_EQ(item::find_component("damage"), comp::kDamage);
    EXPECT_EQ(item::find_component("minecraft:damage"), comp::kDamage);
    EXPECT_EQ(item::find_component("other:damage"), item::kNoComponent);
    EXPECT_EQ(item::component_name(comp::kDamage), "minecraft:damage");
    EXPECT_TRUE(item::has_codec(comp::kDamage) && item::has_codec(comp::kUnbreakable) && !item::has_codec(comp::kFood));

    // A patch the server makes: over another one, a value set, one removed, one put back.
    PatchId base = 0;
    {
        base::ByteBuf tag;
        nbt::SnbtError why;
        ASSERT_TRUE(nbt::read_snbt_fully("{damage:3,max_damage:10,\"!lore\":{}}", tag, why));
        nbt::Doc doc;
        ASSERT_EQ(doc.parse({tag.data, tag.len}, nbt::Form::kNameless, nbt::kTrusted), nbt::Error::kNone);
        ASSERT_EQ(item::patch_from_nbt(doc.root(), base), Verdict::kOk);
    }
    item::PatchBuilder b(base);
    const u8 seven[] = {7};
    b.set(comp::kDamage, seven);
    b.remove(comp::kMaxDamage);
    b.erase(comp::kLore);
    PatchId made = 0;
    ASSERT_EQ(b.build(made), Verdict::kOk);
    EXPECT_EQ(item::patches().find(made, comp::kDamage).value[0], 7);
    EXPECT_EQ(item::patches().find(made, comp::kMaxDamage).state, item::Patches::State::kRemoved);
    EXPECT_EQ(item::patches().find(made, comp::kLore).state, item::Patches::State::kInherit);
    item::patches().release(made);
    item::patches().release(base);
    EXPECT_EQ(item::patches().live(), held);
}

// What an item has of its own: the generated table of default components, read by the same
// code that reads a command's `stone[...]` - so every default of vanilla's is a test of it.
TEST(ComponentCodec, StacksInStacksAndTheRestOfTheData) {
    const u32 held = item::patches().live();  // (the recipes keep a few stacks with components)
    // ItemStackTemplate.CODEC: a compound - or the item's name alone. A count of 1 and no
    // components are a field's default: left out.
    EXPECT_EQ(through(comp::kSulfurCubeContent, "\"stone\""), "{id:\"minecraft:stone\"}");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"stone\",count:1,components:{}}"), "{id:\"minecraft:stone\"}");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"diamond_sword\",count:2b,components:{damage:3,\"!lore\":{}}}"),
              "{components:{\"!minecraft:lore\":{},\"minecraft:damage\":3},count:2,id:\"minecraft:diamond_sword\"}");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"air\"}"), "malformed");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"stone\",count:100}"), "malformed");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"stone\",count:0}"), "malformed");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"stone\",components:{damage:-1}}"), "malformed");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"stone\",components:{food:{}}}"), "unsupported");
    EXPECT_EQ(through(comp::kSulfurCubeContent, "{id:\"stone\",components:{creative_slot_lock:{}}}"), "malformed");  // no Codec
    // container: {slot, item}, whatever order they come in; a slot twice is its last.
    EXPECT_EQ(through(comp::kContainer, "[{slot:2,item:{id:\"stone\",count:5}},{slot:0,item:\"dirt\"},{slot:2,item:\"sand\"}]"),
              "[{item:{id:\"minecraft:dirt\"},slot:0},{item:{id:\"minecraft:sand\"},slot:2}]");
    EXPECT_EQ(through(comp::kContainer, "[]"), "[]");
    EXPECT_EQ(through(comp::kContainer, "[{slot:256,item:\"dirt\"}]"), "malformed");
    EXPECT_EQ(through(comp::kContainer, "[{slot:0}]"), "malformed");
    EXPECT_EQ(through(comp::kBundleContents, "[\"stone\",{id:\"shulker_box\",components:{container:[{slot:1,item:\"dirt\"}]}}]"),
              "[{id:\"minecraft:stone\"},{components:{\"minecraft:container\":[{item:{id:\"minecraft:dirt\"},slot:1}]},id:"
              "\"minecraft:shulker_box\"}]");
    EXPECT_EQ(through(comp::kChargedProjectiles, "[{id:\"arrow\"}]"), "[{id:\"minecraft:arrow\"}]");
    EXPECT_EQ(through(comp::kPotDecorations, "{front:\"brick\",back:{id:\"angler_pottery_sherd\"}}"),
              "{back:{id:\"minecraft:angler_pottery_sherd\"},front:{id:\"minecraft:brick\"}}");
    EXPECT_EQ(through(comp::kPotDecorations, "{}"), "{}");
    EXPECT_EQ(through(comp::kBees, "[{entity_data:{id:\"bee\",NoAI:1b},ticks_in_hive:5,min_ticks_in_hive:2400}]"),
              "[{entity_data:{NoAI:1b,id:\"minecraft:bee\"},min_ticks_in_hive:2400,ticks_in_hive:5}]");
    EXPECT_EQ(through(comp::kBees, "[{entity_data:{id:\"bee\"},ticks_in_hive:5}]"), "malformed");
    // One entry of a registry, one constant of an enum: by name.
    EXPECT_EQ(through(comp::kWolfVariant, "\"snowy\""), "\"minecraft:snowy\"");
    EXPECT_EQ(through(comp::kWolfVariant, "\"nope\""), "malformed");
    EXPECT_EQ(through(comp::kVillagerVariant, "\"minecraft:taiga\""), "\"minecraft:taiga\"");
    EXPECT_EQ(through(comp::kRabbitVariant, "\"evil\""), "\"evil\"");
    EXPECT_EQ(through(comp::kRabbitVariant, "\"minecraft:evil\""), "malformed");
    EXPECT_EQ(through(comp::kTropicalFishPattern, "\"stripey\""), "\"stripey\"");
    EXPECT_EQ(through(comp::kHorseVariant, "\"dark_brown\""), "\"dark_brown\"");
    EXPECT_EQ(through(comp::kFoxVariant, "1"), "malformed");
    // The rest.
    EXPECT_EQ(through(comp::kCustomModelData, "{floats:[1.5f,2],flags:[1b,0b,5],strings:[\"a\"],colors:[255]}"),
              "{colors:[255],flags:[1b,0b,1b],floats:[1.5f,2.0f],strings:[\"a\"]}");
    EXPECT_EQ(through(comp::kCustomModelData, "{}"), "{}");
    EXPECT_EQ(through(comp::kCustomModelData, "{floats:[\"a\"]}"), "malformed");
    EXPECT_EQ(through(comp::kTrim, "{material:\"gold\",pattern:\"minecraft:coast\"}"),
              "{material:\"minecraft:gold\",pattern:\"minecraft:coast\"}");
    EXPECT_EQ(through(comp::kTrim, "{material:\"gold\"}"), "malformed");
    EXPECT_EQ(through(comp::kTrim, "{material:{asset_name:\"x\"},pattern:\"coast\"}"), "unsupported");
    EXPECT_EQ(through(comp::kProvidesTrimMaterial, "\"iron\""), "\"minecraft:iron\"");
    EXPECT_EQ(through(comp::kProvidesBannerPatterns, "\"#pattern_item/globe\""), "\"#minecraft:pattern_item/globe\"");
    EXPECT_EQ(through(comp::kProvidesBannerPatterns, "\"#no_such_tag\""), "malformed");
    EXPECT_EQ(through(comp::kProvidesBannerPatterns, "\"globe\""), "\"minecraft:globe\"");
    EXPECT_EQ(through(comp::kProvidesBannerPatterns, "[\"globe\",\"creeper\"]"), "[\"minecraft:globe\",\"minecraft:creeper\"]");
    EXPECT_EQ(through(comp::kProvidesBannerPatterns, "[]"), "[]");
    EXPECT_EQ(through(comp::kLodestoneTracker, "{target:{dimension:\"overworld\",pos:[I;-5,70,1000]},tracked:0b}"),
              "{target:{dimension:\"minecraft:overworld\",pos:[I;-5,70,1000]},tracked:0b}");
    EXPECT_EQ(through(comp::kLodestoneTracker, "{target:{dimension:\"overworld\",pos:[1,-64,-3]}}"),
              "{target:{dimension:\"minecraft:overworld\",pos:[I;1,-64,-3]}}");
    EXPECT_EQ(through(comp::kLodestoneTracker, "{}"), "{}");
    EXPECT_EQ(through(comp::kLodestoneTracker, "{target:{dimension:\"overworld\",pos:[I;1,2]}}"), "malformed");
    EXPECT_EQ(through(comp::kBreakSound, "\"item.shield.break\""), "\"minecraft:item.shield.break\"");
    EXPECT_EQ(through(comp::kBreakSound, "\"no.such.sound\""), "malformed");
    EXPECT_EQ(through(comp::kBreakSound, "{sound_id:\"my:sound\",range:16}"), "{range:16.0f,sound_id:\"my:sound\"}");
    EXPECT_EQ(through(comp::kIntangibleProjectile, "{}"), "{}");
    EXPECT_EQ(through(comp::kRecipes, "[\"stick\",\"my:recipe\"]"), "[\"minecraft:stick\",\"my:recipe\"]");
    EXPECT_EQ(through(comp::kRecipes, "[]"), "[]");
    EXPECT_EQ(through(comp::kContainerLoot, "{loot_table:\"chests/simple_dungeon\",seed:5}"),
              "{loot_table:\"minecraft:chests/simple_dungeon\",seed:5L}");
    EXPECT_EQ(through(comp::kMapDecorations, "{a:{type:\"red_x\",x:1,z:2.5d,rotation:90}}"),
              "{a:{rotation:90.0f,type:\"minecraft:red_x\",x:1.0d,z:2.5d}}");
    EXPECT_EQ(through(comp::kMapDecorations, "{a:{type:\"nope\",x:1,z:2,rotation:0}}"), "malformed");
    EXPECT_EQ(through(comp::kDebugStickState, "{oak_stairs:\"facing\"}"), "{\"minecraft:oak_stairs\":\"facing\"}");
    // profile: a name alone, a whole profile, any part of one; its properties as a list or as a map.
    EXPECT_EQ(through(comp::kProfile, "\"Notch\""), "{name:\"Notch\"}");
    EXPECT_EQ(through(comp::kProfile,
                      "{id:[I;1,2,3,4],name:\"Bob\",properties:[{name:\"textures\",value:\"abc\",signature:\"s\"}],"
                      "texture:\"my:skin\",model:\"slim\"}"),
              "{id:[I;1,2,3,4],model:\"slim\",name:\"Bob\",properties:[{name:\"textures\",signature:\"s\",value:\"abc\"}],"
              "texture:\"my:skin\"}");
    EXPECT_EQ(through(comp::kProfile, "{properties:{textures:[\"a\",\"b\"]},id:[1,2,3,4]}"),
              "{id:[I;1,2,3,4],properties:[{name:\"textures\",value:\"a\"},{name:\"textures\",value:\"b\"}]}");
    EXPECT_EQ(through(comp::kProfile, "{}"), "{}");
    EXPECT_EQ(through(comp::kProfile, "{model:\"wide\",cape:\"cape\"}"), "{cape:\"minecraft:cape\",model:\"wide\"}");
    EXPECT_EQ(through(comp::kProfile, "{name:\"has space\"}"), "malformed");
    EXPECT_EQ(through(comp::kProfile, "\"SeventeenLettersXY\""), "malformed");
    EXPECT_EQ(through(comp::kProfile, "{id:[I;1,2,3]}"), "malformed");
    EXPECT_EQ(through(comp::kProfile, "{model:\"fat\"}"), "malformed");
    // lock: an ItemPredicate - which items, how many, with which components exactly.
    EXPECT_EQ(through(comp::kLock, "{items:\"#logs\",count:{min:1,max:1},components:{damage:3b}}"),
              "{components:{\"minecraft:damage\":3},count:1,items:\"#minecraft:logs\"}");
    EXPECT_EQ(through(comp::kLock, "{items:[\"stone\"],count:{min:2}}"), "{count:{min:2},items:\"minecraft:stone\"}");
    EXPECT_EQ(through(comp::kLock, "{items:[\"stone\",\"dirt\"],count:{}}"), "{items:[\"minecraft:stone\",\"minecraft:dirt\"]}");
    EXPECT_EQ(through(comp::kLock, "{}"), "{}");
    EXPECT_EQ(through(comp::kLock, "{count:{min:5,max:1}}"), "malformed");
    EXPECT_EQ(through(comp::kLock, "{items:\"#no_such_tag\"}"), "malformed");
    EXPECT_EQ(through(comp::kLock, "{components:{damage:-1}}"), "malformed");
    EXPECT_EQ(through(comp::kLock, "{predicates:{damage:{}}}"), "unsupported");
    // How many of the 122 are read (docs/architecture.md says so too).
    {
        base::ByteBuf scratch;
        u32 read = 0;
        for (u32 type = 0; type < comp::kCount; ++type) read += item::check_value(type, {}, scratch) != Verdict::kUnsupported;
        EXPECT_EQ(read, 93u);
    }
    // The three that only travel are no part of a tag.
    EXPECT_FALSE(item::has_codec(comp::kCreativeSlotLock) || item::has_codec(comp::kAdditionalTradeCost) ||
                 item::has_codec(comp::kMapPostProcessing));
    EXPECT_EQ(through(comp::kAdditionalTradeCost, "5"), "malformed");
    EXPECT_EQ(item::patches().live(), held);
}

TEST(ComponentCodec, DefaultsAndWhatAPatchNeedNotSay) {
    const u32 held = item::patches().live();
    EXPECT_EQ(item::unread_defaults(), 0u);  // every default of a component that is read here is read
    const item::Default size = item::default_of(it::kStone, comp::kMaxStackSize);
    ASSERT_TRUE(size.has && size.read);
    EXPECT_EQ(Bytes(size.value.begin(), size.value.end()), vi(64));
    EXPECT_TRUE(item::default_of(it::kDiamondSword, comp::kTool).has);  // a component that is not read: known to be there
    EXPECT_FALSE(item::default_of(it::kDiamondSword, comp::kTool).read);
    EXPECT_FALSE(item::default_of(it::kStone, comp::kDamage).has);

    const auto stack = [](std::string_view snbt) {
        base::ByteBuf tag;
        nbt::SnbtError why;
        EXPECT_TRUE(nbt::read_snbt_fully(snbt, tag, why)) << snbt;
        nbt::Doc doc;
        EXPECT_EQ(doc.parse({tag.data, tag.len}, nbt::Form::kNameless, nbt::kTrusted), nbt::Error::kNone);
        ItemStack s;
        EXPECT_EQ(item::stack_from_nbt(doc.root(), s), Verdict::kOk) << snbt;
        return s;
    };
    // PatchedDataComponentMap: what the item says anyway is no part of a patch, nor is taking
    // away what it does not have.
    EXPECT_EQ(stack("{id:\"stone\",components:{max_stack_size:64,lore:[],rarity:\"common\"}}").patch(), 0u);
    EXPECT_EQ(stack("{id:\"stone\",components:{\"!damage\":{}}}").patch(), 0u);
    EXPECT_NE(stack("{id:\"stone\",components:{max_stack_size:16}}").patch(), 0u);
    EXPECT_NE(stack("{id:\"stone\",components:{\"!lore\":{}}}").patch(), 0u);
    EXPECT_NE(stack("{id:\"diamond_sword\",components:{\"!tool\":{}}}").patch(), 0u);
    {
        const ItemStack worn = stack("{id:\"diamond_sword\",components:{damage:0,max_damage:1561,unbreakable:{}}}");
        ASSERT_NE(worn.patch(), 0u);
        ASSERT_EQ(item::patches().entries(worn.patch()).size(), 1u);  // the two it has anyway are gone
        EXPECT_EQ(item::patches().entries(worn.patch())[0].type, comp::kUnbreakable);
        EXPECT_EQ(item::effective(worn, comp::kMaxDamage).value.size(), vi(1561).size());
    }
    // A stack a client sends is held against its item the same way.
    {
        const Bytes sent =
            vi(1) + vi(it::kStone) + untrusted_patch({{comp::kMaxStackSize, vi(64)}, {comp::kRarity, vi(0)}}, {comp::kDamage});
        proto::Reader r(sent.data(), sent.size());
        ItemStack s;
        ASSERT_EQ(item::read_untrusted(r, s), Verdict::kOk);
        EXPECT_EQ(s.patch(), 0u);
    }

    // The items that name themselves by what they hold.
    EXPECT_EQ(item::computed_name_key(stack("{id:\"potion\"}")), "item.minecraft.potion.effect.empty");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"potion\",components:{potion_contents:\"long_swiftness\"}}")),
              "item.minecraft.potion.effect.swiftness");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"tipped_arrow\",components:{potion_contents:{potion:\"strong_healing\"}}}")),
              "item.minecraft.tipped_arrow.effect.healing");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"splash_potion\",components:{potion_contents:{custom_effects:[{id:\"speed\","
                                            "hidden_effect:{duration:5}}],custom_name:\"x\"}}}")),
              "item.minecraft.splash_potion.effect.x");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"shield\",components:{base_color:\"red\"}}")), "item.minecraft.shield.red");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"shield\"}")), "");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"stone\"}")), "");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"compass\"}")), "");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"compass\",components:{lodestone_tracker:{}}}")),
              "item.minecraft.lodestone_compass");
    std::string whose;
    EXPECT_EQ(item::computed_name_key(stack("{id:\"player_head\",components:{profile:\"Notch\"}}"), &whose),
              "block.minecraft.player_head.named");
    EXPECT_EQ(whose, "Notch");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"player_head\",components:{profile:{id:[I;1,2,3,4]}}}")), "");
    EXPECT_EQ(item::computed_name_key(stack("{id:\"player_head\"}")), "");
    EXPECT_EQ(item::book_title(stack("{id:\"written_book\",components:{written_book_content:{title:\"T\",author:\"A\"}}}")), "T");
    EXPECT_EQ(item::book_title(stack("{id:\"written_book\",components:{written_book_content:{title:\"  \",author:\"A\"}}}")), "");
    EXPECT_EQ(item::patches().live(), held);
}
