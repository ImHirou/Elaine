// Item stacks and component patches: what a client may send, what is kept of it, and what a
// stack answers about itself. The byte strings are written out by hand from the components'
// StreamCodecs in the 26.3 jar, not produced by the code under test.
#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "item/stack.hpp"
#include "proto/packets.hpp"

#include PROTO_GEN_ITEMS_HPP

using namespace proto;
using item::ItemStack;
using item::PatchId;
using item::Verdict;
namespace comp = proto::gen::component;
namespace it = proto::gen::item;

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
    // custom_name: a text component (NBT). Well-formed here or not, it is not this server's to say.
    Read r = read_patch(untrusted_patch({{comp::kCustomName, {0x08, 0x00, 0x02, 'h', 'i'}}}));
    EXPECT_EQ(r.verdict, Verdict::kUnsupported);
    EXPECT_TRUE(r.consumed);  // the length says where it ends: what follows the stack can still be read
    EXPECT_TRUE(r.wire.empty());
    // One unsupported component spoils the stack, whatever else it carries...
    EXPECT_EQ(read_patch(untrusted_patch({{comp::kDamage, vi(1)}, {comp::kCustomData, {0x0A, 0x00}}})).verdict,
              Verdict::kUnsupported);
    // ...but removing a component needs no understanding of it.
    EXPECT_EQ(read_patch(untrusted_patch({}, {comp::kCustomName, comp::kAttributeModifiers})).verdict, Verdict::kOk);
    // Malformed beats unsupported when it is noticed.
    EXPECT_EQ(read_patch(untrusted_patch({{comp::kCustomName, {0x08}}, {comp::kDamage, vi(0xFFFFFFFF)}})).verdict,
              Verdict::kMalformed);
}

TEST(Patches, TooLargeIsUnsupported) {
    // 400 custom potion effects: fine for vanilla, more than a stack may carry here.
    Bytes effects = vi(400);
    for (u32 i = 0; i < 400; ++i) effects = effects + vi(i % 40) + Bytes{0, 1, 0, 1, 1, 0};
    const Bytes potion = Bytes{0, 0} + effects + Bytes{0};
    ASSERT_GT(potion.size(), item::kMaxPatchBytes);
    const Read r = read_patch(untrusted_patch({{comp::kPotionContents, potion}}));
    EXPECT_EQ(r.verdict, Verdict::kUnsupported);
    EXPECT_TRUE(r.consumed);
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
    // Everything set: colour, one effect with a hidden effect below it, a name.
    const Bytes effect = vi(3) /* effect */ + vi(300) /* amplifier, clamped */ + vi(600) + Bytes{1, 1, 0} +
                         Bytes{1} /* hidden */ + vi(1) + vi(0xFFFFFFFF) /* infinite */ + Bytes{0, 9, 1} + Bytes{0};
    const Bytes kept = vi(3) + vi(255) + vi(600) + Bytes{1, 1, 0} + Bytes{1} + vi(1) + vi(0xFFFFFFFF) + Bytes{0, 1, 1} + Bytes{0};
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

    // tooltip_display: the hidden components are a set.
    expect_kept(comp::kTooltipDisplay, Bytes{1} + vi(3) + vi(comp::kLore) + vi(comp::kDamage) + vi(comp::kLore),
                Bytes{1} + vi(2) + vi(comp::kDamage) + vi(comp::kLore));
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
    EXPECT_EQ(read(vi(1) + vi(it::kStone) + untrusted_patch({{comp::kCustomName, {0x08, 0x00, 0x00}}}) + Bytes{0x77}, s, &done),
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
