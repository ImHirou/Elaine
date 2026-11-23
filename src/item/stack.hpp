// SPDX-License-Identifier: GPL-3.0-or-later
// ItemStack: what a slot holds - an item, a count and (rarely) a component patch. Eight bytes,
// so that an inventory is a small dense array and a stack is passed around by value.
//
// The patch is an id into item::Patches (components.hpp), which keeps every distinct patch once
// and counts who holds it: copying a stack takes a reference, destroying one gives it back, and
// two stacks have the same components exactly when their ids are equal. For the stacks that
// carry no patch - nearly all - copy and destroy are a test of one field.
//
// The methods are vanilla's (net.minecraft.world.item.ItemStack) and answer as it does. A stack
// whose count reached zero is empty - it reports no item and no count - but still remembers
// what it was until it is overwritten, as a Java ItemStack does.
#pragma once

#include <algorithm>

#include "item/components.hpp"
#include "proto/gen/current.hpp"
#include "proto/packets.hpp"
#include "world/block.hpp"

#include PROTO_GEN_ENTITIES_HPP

namespace item {

namespace equip = proto::gen::equipment_slot;  // equip::kHead, ...: EquipmentSlot ordinals
inline constexpr u32 kNoEquipSlot = proto::gen::kNoEquipSlot;
inline constexpr u32 kMaxCount = 99;  // ItemStack.CODEC: a count is 1..99

class ItemStack {
  public:
    constexpr ItemStack() = default;
    ItemStack(u32 item, u32 count) : item_(static_cast<u16>(item)), count_(static_cast<u8>(count)) {
        DCHECK(item < world::item_count() && count <= kMaxCount);
    }
    // Takes over the caller's reference to `patch`.
    ItemStack(u32 item, u32 count, PatchId patch) : item_(static_cast<u16>(item)), count_(static_cast<u8>(count)), patch_(patch) {
        DCHECK(item < world::item_count() && count <= kMaxCount);
    }
    ItemStack(const ItemStack& o) : item_(o.item_), count_(o.count_), patch_(o.patch_) {
        if (UNLIKELY(patch_)) patches().retain(patch_);
    }
    ItemStack(ItemStack&& o) noexcept : item_(o.item_), count_(o.count_), patch_(o.patch_) { o.forget(); }
    ItemStack& operator=(const ItemStack& o) {
        if (UNLIKELY(o.patch_)) patches().retain(o.patch_);  // first: o may be *this
        if (UNLIKELY(patch_)) patches().release(patch_);
        item_ = o.item_;
        count_ = o.count_;
        patch_ = o.patch_;
        return *this;
    }
    ItemStack& operator=(ItemStack&& o) noexcept {
        if (this == &o) return *this;
        if (UNLIKELY(patch_)) patches().release(patch_);
        item_ = o.item_;
        count_ = o.count_;
        patch_ = o.patch_;
        o.forget();
        return *this;
    }
    ~ItemStack() {
        if (UNLIKELY(patch_)) patches().release(patch_);
    }

    // ---- what it is. Air, or nothing of something, is empty (ItemStack.isEmpty).
    bool empty() const { return count_ == 0 || item_ == 0; }
    u32 item() const { return empty() ? 0 : item_; }
    u32 count() const { return empty() ? 0 : count_; }
    PatchId patch() const { return empty() ? 0 : patch_; }
    bool has_patch() const { return patch() != 0; }
    const world::ItemInfo& info() const { return world::item_info(item_); }

    // ---- counts
    void set_count(u32 n) {
        DCHECK(n <= kMaxCount);
        count_ = static_cast<u8>(n);
    }
    void grow(u32 n) { set_count(count() + n); }
    void shrink(u32 n) { set_count(count() - std::min(n, count())); }
    // copyWithCount: the same item and components, `n` of them.
    ItemStack with_count(u32 n) const {
        if (empty()) return {};
        ItemStack s(*this);
        s.set_count(n);
        return s;
    }
    // split: takes up to `n` off this stack and returns them.
    ItemStack split(u32 n) {
        n = std::min(n, count());
        ItemStack s = with_count(n);
        shrink(n);
        return s;
    }
    // copyAndClear: the whole stack, leaving nothing behind.
    ItemStack take() {
        ItemStack s = empty() ? ItemStack() : std::move(*this);
        *this = {};
        return s;
    }

    // ---- components: the patch where it says something, the item's defaults where it does not
    // getMaxStackSize: MAX_STACK_SIZE, or 1 without one.
    u32 max_stack() const {
        if (LIKELY(!patch_)) return info().max_stack;
        const auto f = patches().find(patch_, component::kMaxStackSize);
        return f.state == Patches::State::kSet ? varint_of(f) : f.state == Patches::State::kRemoved ? 1 : info().max_stack;
    }
    // isDamageableItem: MAX_DAMAGE and DAMAGE and not UNBREAKABLE.
    bool damageable() const {
        if (LIKELY(!patch_)) return (info().flags & kDamageBits) == kDamageBits;
        return has(component::kMaxDamage, world::item_flag::kHasMaxDamage) &&
               has(component::kDamage, world::item_flag::kHasDamage) && !has(component::kUnbreakable, 0);
    }
    // ItemStack.canBeHurtBy, the other way round, for what an item on the ground meets: an
    // explosion (item_flag::kExplosionProof), fire and lava (kFireProof). DAMAGE_RESISTANT names
    // a tag of damage types - the item's own, as long as no patch has taken it away. (One that
    // a patch gives to an item which has none is not read: such a stack burns like any other.)
    bool resists(u32 flag) const {
        if (LIKELY(!(info().flags & flag))) return false;
        return !patch_ || patches().find(patch_, component::kDamageResistant).state != Patches::State::kRemoved;
    }
    // ItemStack.validateComponents: what can be worn down is one to a stack.
    bool damageable_and_stackable() const {
        const bool wears = patch_ ? has(component::kMaxDamage, world::item_flag::kHasMaxDamage)
                                  : (info().flags & world::item_flag::kHasMaxDamage) != 0;
        return wears && max_stack() > 1;
    }
    // isDamaged: no item is damaged by default, so only a patch can say so.
    bool damaged() const {
        if (LIKELY(!patch_) || !damageable()) return false;
        const auto f = patches().find(patch_, component::kDamage);
        return f.state == Patches::State::kSet && varint_of(f) > 0;  // getDamageValue clamps to 0..MAX_DAMAGE, which is >= 1
    }
    bool stackable() const { return max_stack() > 1 && !(damageable() && damaged()); }
    // isEnchanted: ENCHANTMENTS holds at least one. Empty by default for every item.
    bool enchanted() const {
        if (LIKELY(!patch_)) return false;
        const auto f = patches().find(patch_, component::kEnchantments);
        return f.state == Patches::State::kSet && varint_of(f) > 0;  // the value starts with the entry count
    }
    // The slot of its EQUIPPABLE component, kNoEquipSlot without one. (A patch cannot set the
    // component yet - it is not among those the server reads - only remove it.)
    u32 equip_slot() const {
        if (empty()) return kNoEquipSlot;
        if (UNLIKELY(patch_) && patches().find(patch_, component::kEquippable).state == Patches::State::kRemoved)
            return kNoEquipSlot;
        return info().equip_slot;
    }
    // Equippable.canBeEquippedBy(a player), for a stack that has an equip slot.
    bool players_may_wear() const { return !(info().flags & world::item_flag::kEquipRestricted); }
    // getAttackAnimation: ATTACK_ANIMATION, or SwingAnimation.DEFAULT without one - how the arm
    // of whoever punches with this in hand swings. (As with EQUIPPABLE, a patch can only remove it.)
    proto::Swing attack_swing() const {
        if (empty()) return {};
        if (UNLIKELY(patch_) && patches().find(patch_, component::kAttackAnimation).state == Patches::State::kRemoved) return {};
        const u8 packed = info().attack_swing;
        return {static_cast<u8>(packed >> proto::gen::kSwingTypeShift), static_cast<u8>(packed & proto::gen::kSwingTicksMask)};
    }

  private:
    static constexpr u32 kDamageBits = world::item_flag::kHasMaxDamage | world::item_flag::kHasDamage;
    static u32 varint_of(const Patches::Found& f) {
        u32 v = 0;
        proto::get_varint_slow(f.value.data(), f.value.data() + f.value.size(), v);
        return v;
    }
    bool has(u32 type, u32 default_flag) const {
        const auto f = patches().find(patch_, type);
        return f.state == Patches::State::kSet || (f.state == Patches::State::kInherit && (info().flags & default_flag) != 0);
    }
    void forget() {
        item_ = 0;
        count_ = 0;
        patch_ = 0;
    }

    u16 item_ = 0;
    u8 count_ = 0;
    PatchId patch_ = 0;
};
static_assert(sizeof(ItemStack) == 8);

// ItemStack.isSameItem
inline bool same_item(const ItemStack& a, const ItemStack& b) { return a.item() == b.item(); }
// ItemStack.isSameItemSameComponents: what may share a slot.
inline bool same_item_same_components(const ItemStack& a, const ItemStack& b) {
    return a.item() == b.item() && a.patch() == b.patch();
}
// ItemStack.matches: the same in every respect.
inline bool matches(const ItemStack& a, const ItemStack& b) { return a.count() == b.count() && same_item_same_components(a, b); }

// EquipmentSlot.Type.HUMANOID_ARMOR: feet, legs, chest, head.
inline bool humanoid_armor(u32 slot) { return slot >= equip::kFeet && slot <= equip::kHead; }

// ---------------------------------------------------------------------------- wire

// A stack as packets carry it to a client (proto::put_item).
// What a stack costs whoever is sent it, if it is heavy (kHeavyPatchBytes): its patch's bytes. 0 for the rest.
inline u32 heavy_bytes(const ItemStack& s) {
    if (LIKELY(!s.patch())) return 0;
    const usize n = patches().wire(s.patch()).size();
    return n > kHeavyPatchBytes ? static_cast<u32>(n) : 0;
}

inline proto::ItemView view(const ItemStack& s) {
    if (s.empty()) return {};
    return {s.count(), s.item(), s.patch() ? patches().wire(s.patch()) : std::span<const u8>{}};
}

// A stack as a client sends it: ItemStack.OPTIONAL_UNTRUSTED_STREAM_CODEC, read the way
// ItemStack.validatedStreamCodec reads it for set_creative_mode_slot - count, item, then a
// patch whose values are length-prefixed. kOk: `out` is the stack (empty for a count <= 0 or
// for air). kUnsupported: a well-formed stack with a component this server does not read yet;
// the reader stands behind it and `out` is empty.
Verdict read_untrusted(proto::Reader& r, ItemStack& out);
// The same form written: what read_untrusted takes back. It is how a stack is kept on disk -
// whatever a file says goes through the door a client's stacks go through.
void write_untrusted(base::ByteBuf& out, const ItemStack& s);

}  // namespace item
