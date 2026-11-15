#include "server/inventory.hpp"

#include <bit>

#include "proto/frame.hpp"
#include "proto/packets.hpp"

namespace server {

using namespace proto;
namespace equip = item::equip;
using item::matches;
using item::same_item;
using item::same_item_same_components;

namespace {

constexpr u32 kNoLimit = 0x7FFFFFFF;  // Integer.MAX_VALUE where vanilla passes "as many as there are"
constexpr u32 kContainerLimit = 99;   // Container.getMaxStackSize()

// The slots of InventoryMenu, by what they are.
enum class Kind : u8 { kResult, kCraft, kArmor, kPlain, kOffhand };
Kind kind_of(i32 slot) {
    if (slot == menu_slot::kResult) return Kind::kResult;
    if (slot < menu_slot::kCraftEnd) return Kind::kCraft;
    if (slot < menu_slot::kArmorEnd) return Kind::kArmor;
    return slot == menu_slot::kOffhand ? Kind::kOffhand : Kind::kPlain;
}
// InventoryMenu.SLOT_IDS: slots 5..8 are head, chest, legs, feet.
u32 armor_slot(i32 slot) { return equip::kHead - static_cast<u32>(slot - menu_slot::kArmorFirst); }

// The Inventory.getMaxStackSize(stack) of Container: what one slot takes of it.
u32 container_max(const ItemStack& s) { return std::min(kContainerLimit, s.max_stack()); }

}  // namespace

// ---------------------------------------------------------------------------- Inventory

i32 Inventory::free_slot() const {
    for (u32 i = 0; i < inv::kItems; ++i)
        if (items[i].empty()) return static_cast<i32>(i);
    return -1;
}

i32 Inventory::slot_with_remaining_space(const ItemStack& s) const {
    // hasRemainingSpaceForItem: the same thing, stackable, and not full.
    const auto room = [&](const ItemStack& in) {
        return !in.empty() && same_item_same_components(in, s) && in.stackable() && in.count() < container_max(in);
    };
    if (room(items[selected])) return selected;
    if (room(items[inv::kOffhand])) return inv::kOffhand;
    for (u32 i = 0; i < inv::kItems; ++i)
        if (room(items[i])) return static_cast<i32>(i);
    return -1;
}

i32 Inventory::find_matching(const ItemStack& s) const {
    for (u32 i = 0; i < inv::kItems; ++i)
        if (!items[i].empty() && same_item_same_components(s, items[i])) return static_cast<i32>(i);
    return -1;
}

i32 Inventory::suitable_hotbar_slot() const {
    for (u32 i = 0; i < inv::kHotbarSize; ++i) {
        const u32 slot = (selected + i) % inv::kHotbarSize;
        if (items[slot].empty()) return static_cast<i32>(slot);
    }
    for (u32 i = 0; i < inv::kHotbarSize; ++i) {
        const u32 slot = (selected + i) % inv::kHotbarSize;
        if (!items[slot].enchanted()) return static_cast<i32>(slot);
    }
    return selected;
}

bool Inventory::add(i32 slot, ItemStack& stack, bool infinite) {
    if (stack.empty()) return false;
    if (stack.damaged()) {  // a damaged item never joins another: it takes a slot of its own
        if (slot == -1) slot = free_slot();
        if (slot >= 0) {
            items[slot] = stack.take();
            return true;
        }
        if (infinite) {
            stack.set_count(0);
            return true;
        }
        return false;
    }
    // addResource, until nothing more goes in.
    u32 before;
    do {
        before = stack.count();
        i32 to = slot;
        if (to == -1) {
            to = slot_with_remaining_space(stack);
            if (to == -1) to = free_slot();
        }
        if (to == -1) break;
        ItemStack& in = items[to];
        const u32 cap = container_max(in.empty() ? stack : in);
        const u32 moved = std::min(stack.count(), cap - std::min(in.count(), cap));
        if (moved == 0) break;
        if (in.empty())
            in = stack.with_count(moved);
        else
            in.grow(moved);
        stack.shrink(moved);
    } while (!stack.empty() && stack.count() < before);
    // Vanilla compares with the count before the *last* round: a stack that went in partly and
    // then stuck reports failure, and in creative its rest is destroyed.
    if (stack.empty()) return true;
    if (infinite) {
        stack.set_count(0);
        return true;
    }
    return false;
}

void Inventory::place_back(ItemStack stack, bool infinite, Effects& fx) {
    while (!stack.empty()) {
        i32 slot = slot_with_remaining_space(stack);
        if (slot == -1) slot = free_slot();
        if (slot == -1) {  // no room: it falls to the ground (Prediction.SERVER_ONLY: the client is told to swing)
            fx.push_back(Effect::drop(std::move(stack), false, false));
            return;
        }
        const u32 room = stack.max_stack() - std::min(items[slot].count(), stack.max_stack());
        if (room == 0) return;  // cannot be: the slot was chosen for having room
        ItemStack part = stack.split(room);
        if (add(slot, part, infinite)) fx.push_back(Effect::inventory_slot(static_cast<u32>(slot), items[slot]));
    }
}

void Inventory::pick_slot(i32 slot) {
    selected = static_cast<u8>(suitable_hotbar_slot());
    std::swap(items[selected], items[slot]);
}

void Inventory::add_and_pick(ItemStack s) {
    selected = static_cast<u8>(suitable_hotbar_slot());
    if (!items[selected].empty()) {
        if (const i32 free = free_slot(); free != -1) items[free] = std::move(items[selected]);
    }
    items[selected] = std::move(s);
}

// ---------------------------------------------------------------------------- slots

ItemStack& Menu::at(i32 slot) {
    DCHECK(slot >= 0 && slot < menu_slot::kCount);
    if (slot >= menu_slot::kHotbarFirst)
        return inv.items[slot == menu_slot::kOffhand ? inv::kOffhand : static_cast<u32>(slot - menu_slot::kHotbarFirst)];
    if (slot >= menu_slot::kInvFirst) return inv.items[slot];
    if (slot >= menu_slot::kArmorFirst)
        return inv.items[inv::kHead - static_cast<u32>(slot - menu_slot::kArmorFirst)];  // 5..8 = 39..36
    return slot == menu_slot::kResult ? result_ : craft[slot - menu_slot::kCraftFirst];
}

i32 Menu::slot_of(u32 index) {
    if (index < inv::kHotbarSize) return menu_slot::kHotbarFirst + static_cast<i32>(index);
    if (index < inv::kItems) return static_cast<i32>(index);
    if (index <= inv::kHead) return menu_slot::kArmorFirst + static_cast<i32>(inv::kHead - index);
    return index == inv::kOffhand ? menu_slot::kOffhand : -1;
}

u32 Menu::slot_limit(i32 slot) const { return kind_of(slot) == Kind::kArmor ? 1 : kContainerLimit; }

u32 Menu::max_for(i32 slot, const ItemStack& s) const { return std::min(slot_limit(slot), s.max_stack()); }

bool Menu::may_place(i32 slot, const ItemStack& s) const {
    switch (kind_of(slot)) {
        case Kind::kResult: return false;
        // LivingEntity.isEquippableInSlot: the item's EQUIPPABLE names this slot and admits players.
        case Kind::kArmor: return s.equip_slot() == armor_slot(slot) && s.players_may_wear();
        default: return true;
    }
}

bool Menu::may_pickup(i32 slot, const Ctx& cx) const {
    if (kind_of(slot) != Kind::kArmor || cx.creative) return true;
    // ArmorSlot: what carries an enchantment with the prevent_armor_change effect stays on. In
    // the vanilla data that is the curse of binding alone.
    const ItemStack& s = at(slot);
    if (!s.has_patch()) return true;
    const auto found = item::patches().find(s.patch(), item::component::kEnchantments);
    if (found.state != item::Patches::State::kSet) return true;
    Reader r(found.value);  // ItemEnchantments: count, then (enchantment, level)
    for (u32 n = r.varint(); n > 0 && r.ok; --n) {
        if (r.varint() == reg::enchantment::kBindingCurse) return false;
        r.varint();
    }
    return true;
}

// LivingEntity.onEquipItem, for the two kinds of slot that call it (armour, off hand).
void Menu::equip_hook(u32 slot, const ItemStack& old, const ItemStack& now, const Ctx& cx) {
    if (same_item_same_components(old, now)) return;
    if (now.equip_slot() == slot) cx.fx.push_back(Effect::equip_sound(now.info().equip_sound));
}

void Menu::set_by_player(i32 slot, ItemStack s, const Ctx& cx) {
    ItemStack& in = at(slot);
    if (const Kind kind = kind_of(slot); kind == Kind::kArmor)
        equip_hook(armor_slot(slot), in, s, cx);
    else if (kind == Kind::kOffhand)
        equip_hook(equip::kOffhand, in, s, cx);
    in = std::move(s);
}

bool Menu::try_remove(i32 slot, u32 amount, u32 max_amount, const Ctx& cx, ItemStack& taken) {
    if (!may_pickup(slot, cx)) return false;
    ItemStack& in = at(slot);
    // allowModification: a slot that cannot take its own stack back is all or nothing.
    if (!(may_pickup(slot, cx) && may_place(slot, in)) && max_amount < in.count()) return false;
    taken = in.split(std::min(amount, max_amount));
    if (taken.empty()) return false;
    if (in.empty()) {  // setByPlayer(EMPTY, taken): what left the slot is what was "taken off"
        if (const Kind kind = kind_of(slot); kind == Kind::kArmor)
            equip_hook(armor_slot(slot), taken, {}, cx);
        else if (kind == Kind::kOffhand)
            equip_hook(equip::kOffhand, taken, {}, cx);
        in = {};
    }
    return true;
}

ItemStack Menu::safe_take(i32 slot, u32 amount, u32 max_amount, const Ctx& cx) {
    ItemStack taken;
    return try_remove(slot, amount, max_amount, cx, taken) ? std::move(taken) : ItemStack();
}

ItemStack Menu::safe_insert(i32 slot, ItemStack input, u32 amount, const Ctx& cx) {
    if (input.empty() || !may_place(slot, input)) return input;
    ItemStack& in = at(slot);
    const u32 room = max_for(slot, input) - std::min(in.count(), max_for(slot, input));
    const u32 n = std::min({amount, input.count(), room});
    if (n == 0) return input;
    if (in.empty()) {
        set_by_player(slot, input.split(n), cx);
    } else if (same_item_same_components(in, input)) {
        input.shrink(n);
        ItemStack grown = in.with_count(in.count() + n);
        set_by_player(slot, std::move(grown), cx);
    }
    return input;
}

bool Menu::quick_replace(i32 slot, const ItemStack& s) const {
    const ItemStack& in = at(slot);
    return in.empty() || (same_item_same_components(s, in) && in.count() <= s.max_stack());
}

// ---------------------------------------------------------------------------- clicks

bool Menu::move_to(ItemStack& stack, i32 first, i32 end, bool backwards, const Ctx& cx) {
    bool changed = false;
    const i32 step = backwards ? -1 : 1, begin = backwards ? end - 1 : first;
    const auto inside = [&](i32 slot) { return backwards ? slot >= first : slot < end; };
    if (stack.stackable()) {  // first onto stacks of the same thing...
        for (i32 dest = begin; !stack.empty() && inside(dest); dest += step) {
            ItemStack& target = at(dest);
            if (target.empty() || !same_item_same_components(stack, target)) continue;
            const u32 total = target.count() + stack.count(), max = max_for(dest, target);
            if (total <= max) {
                stack.set_count(0);
                target.set_count(total);
                changed = true;
            } else if (target.count() < max) {
                stack.shrink(max - target.count());
                target.set_count(max);
                changed = true;
            }
        }
    }
    if (!stack.empty()) {  // ...then into the first empty slot that takes it
        for (i32 dest = begin; inside(dest); dest += step) {
            if (!at(dest).empty() || !may_place(dest, stack)) continue;
            set_by_player(dest, stack.split(std::min(stack.count(), max_for(dest, stack))), cx);
            changed = true;
            break;
        }
    }
    return changed;
}

ItemStack Menu::quick_move(i32 slot, const Ctx& cx) {
    using namespace menu_slot;
    ItemStack& stack = at(slot);
    if (stack.empty()) return {};
    const ItemStack clicked = stack;
    // LivingEntity.getEquipmentSlotForItem: where its EQUIPPABLE says it goes, the main hand otherwise.
    const u32 wear = clicked.equip_slot() == item::kNoEquipSlot ? equip::kMainhand : clicked.equip_slot();
    bool moved;
    if (slot == kResult) {
        moved = move_to(stack, kInvFirst, kHotbarEnd, true, cx);
    } else if (slot < kArmorEnd) {  // out of the crafting grid or off the body: anywhere
        moved = move_to(stack, kInvFirst, kHotbarEnd, false, cx);
    } else if (item::humanoid_armor(wear) && at(kArmorEnd - 1 - static_cast<i32>(wear - equip::kFeet)).empty()) {
        const i32 to = kArmorEnd - 1 - static_cast<i32>(wear - equip::kFeet);  // 8 - EquipmentSlot.getIndex()
        moved = move_to(stack, to, to + 1, false, cx);
    } else if (wear == equip::kOffhand && at(kOffhand).empty()) {
        moved = move_to(stack, kOffhand, kOffhand + 1, false, cx);
    } else if (slot < kInvEnd) {
        moved = move_to(stack, kHotbarFirst, kHotbarEnd, false, cx);
    } else if (slot < kHotbarEnd) {
        moved = move_to(stack, kInvFirst, kInvEnd, false, cx);
    } else {
        moved = move_to(stack, kInvFirst, kHotbarEnd, false, cx);
    }
    if (!moved) return {};
    if (stack.empty()) {  // setByPlayer(EMPTY, clicked)
        if (const Kind kind = kind_of(slot); kind == Kind::kArmor)
            equip_hook(armor_slot(slot), clicked, {}, cx);
        else if (kind == Kind::kOffhand)
            equip_hook(equip::kOffhand, clicked, {}, cx);
        stack = {};
    }
    if (stack.count() == clicked.count()) return {};
    return clicked;
}

bool Menu::click(i32 slot, i32 button, u32 input, const Ctx& cx) {
    using C = sb::ContainerClick;
    const i32 size = menu_slot::kCount;
    DCHECK(slot < size);  // handleContainerClick: isValidSlotIndex

    if (input == C::kQuickCraft) {
        const u8 expected = quickcraft_status_;
        quickcraft_status_ = static_cast<u8>(button & 3);  // getQuickcraftHeader: 0 start, 1 add a slot, 2 end
        if ((expected != 1 || quickcraft_status_ != 2) && expected != quickcraft_status_) {
            reset_quickcraft();
        } else if (carried.empty()) {
            reset_quickcraft();
        } else if (quickcraft_status_ == 0) {
            quickcraft_type_ = static_cast<i8>((button >> 2) & 3);  // 0 spread evenly, 1 one each, 2 a full stack each
            if (quickcraft_type_ == 0 || quickcraft_type_ == 1 || (quickcraft_type_ == 2 && cx.creative)) {
                quickcraft_status_ = 1;
                quickcraft_slots_ = 0;
            } else {
                reset_quickcraft();
            }
        } else if (quickcraft_status_ == 1) {
            if (slot < 0) return false;  // vanilla indexes its slot list with it
            const u32 n = static_cast<u32>(std::popcount(quickcraft_slots_));
            if (quick_replace(slot, carried) && may_place(slot, carried) && (quickcraft_type_ == 2 || carried.count() > n))
                quickcraft_slots_ |= 1ull << slot;
        } else if (quickcraft_status_ == 2) {
            const u32 n = static_cast<u32>(std::popcount(quickcraft_slots_));
            if (n == 1) {  // a drag over one slot is a click on it
                const i32 only = std::countr_zero(quickcraft_slots_);
                reset_quickcraft();
                return click(only, quickcraft_type_, C::kPickup, cx);
            }
            if (n > 1) {
                ItemStack source = carried;
                if (source.empty()) {
                    reset_quickcraft();
                    return true;
                }
                i32 remaining = static_cast<i32>(carried.count());
                for (u64 left = quickcraft_slots_; left; left &= left - 1) {
                    const i32 to = std::countr_zero(left);
                    if (!(quick_replace(to, carried) && may_place(to, carried) &&
                          (quickcraft_type_ == 2 || carried.count() >= n)))
                        continue;
                    const u32 held = at(to).count();
                    // getQuickCraftPlaceCount
                    const u32 each = quickcraft_type_ == 0 ? source.count() / n : quickcraft_type_ == 1 ? 1 : source.max_stack();
                    const u32 now = std::min(each + held, std::min(source.max_stack(), max_for(to, source)));
                    remaining -= static_cast<i32>(now) - static_cast<i32>(held);
                    set_by_player(to, source.with_count(now), cx);
                }
                source.set_count(static_cast<u32>(std::max(remaining, 0)));
                carried = std::move(source);
            }
            reset_quickcraft();
        } else {
            reset_quickcraft();
        }
        return true;
    }
    if (quickcraft_status_ != 0) {
        reset_quickcraft();
        return true;
    }

    if ((input == C::kPickup || input == C::kQuickMove) && (button == 0 || button == 1)) {
        const bool primary = button == 0;  // ClickAction.PRIMARY
        if (slot == menu_slot::kOutside) {
            if (carried.empty()) return true;
            ItemStack out = primary ? carried.take() : carried.split(1);
            cx.fx.push_back(Effect::drop(std::move(out), true, true));
        } else if (input == C::kQuickMove) {
            if (slot < 0 || !may_pickup(slot, cx)) return true;
            // As long as the slot keeps yielding the same item (it only would for a crafting result).
            ItemStack moved = quick_move(slot, cx);
            while (!moved.empty() && same_item(at(slot), moved)) moved = quick_move(slot, cx);
        } else {
            if (slot < 0) return true;
            ItemStack& clicked = at(slot);
            const u32 put = primary ? carried.count() : 1;  // what a click with something on the cursor puts down
            if (clicked.empty()) {
                if (!carried.empty()) carried = safe_insert(slot, std::move(carried), put, cx);
            } else if (may_pickup(slot, cx)) {
                if (carried.empty()) {  // take all of it, or the larger half
                    const u32 amount = primary ? clicked.count() : (clicked.count() + 1) / 2;
                    ItemStack taken;
                    if (try_remove(slot, amount, kNoLimit, cx, taken)) carried = std::move(taken);
                } else if (may_place(slot, carried)) {
                    if (same_item_same_components(clicked, carried)) {
                        carried = safe_insert(slot, std::move(carried), put, cx);
                    } else if (carried.count() <= max_for(slot, carried)) {  // trade places
                        ItemStack was = clicked;
                        set_by_player(slot, std::move(carried), cx);
                        carried = std::move(was);
                    }
                } else if (same_item_same_components(clicked, carried)) {  // a slot that only gives: top up the cursor
                    const u32 amount = clicked.count(),
                              room = carried.max_stack() - std::min(carried.count(), carried.max_stack());
                    ItemStack taken;
                    if (try_remove(slot, amount, room, cx, taken)) carried.grow(taken.count());
                }
            }
        }
        return true;
    }

    if (input == C::kSwap &&
        ((button >= 0 && button < static_cast<i32>(inv::kHotbarSize)) || button == static_cast<i32>(inv::kOffhand))) {
        if (slot < 0) return false;  // vanilla indexes its slot list with it
        ItemStack& source = inv.items[button];
        ItemStack& target = at(slot);
        if (source.empty() && target.empty()) return true;
        if (&source == &target) return true;  // the key of the very slot under the cursor: nothing moves
        if (source.empty()) {
            if (!may_pickup(slot, cx)) return true;
            ItemStack taken = target;
            set_by_player(slot, {}, cx);
            source = std::move(taken);
        } else if (target.empty()) {
            if (!may_place(slot, source)) return true;
            const u32 max = max_for(slot, source);
            if (source.count() > max)
                set_by_player(slot, source.split(max), cx);
            else
                set_by_player(slot, source.take(), cx);
        } else if (may_pickup(slot, cx) && may_place(slot, source)) {
            const u32 max = max_for(slot, source);
            ItemStack was = target;
            if (source.count() > max) {
                set_by_player(slot, source.split(max), cx);
                if (!inv.add(-1, was, cx.creative)) cx.fx.push_back(Effect::drop(std::move(was), true, true));
            } else {
                set_by_player(slot, source.take(), cx);
                source = std::move(was);
            }
        }
        return true;
    }

    if (input == C::kClone && cx.creative && carried.empty() && slot >= 0) {
        if (!at(slot).empty()) carried = at(slot).with_count(at(slot).max_stack());  // Slot.safeClone
        return true;
    }

    if (input == C::kThrow && carried.empty() && slot >= 0) {
        const u32 amount = button == 0 ? 1 : at(slot).count();
        // With ctrl (button 1): again for as long as the slot refills with the same item - which
        // only a crafting result does.
        for (;;) {
            ItemStack out = safe_take(slot, amount, kNoLimit, cx);
            if (out.empty()) break;  // LivingEntity.drop does nothing with an empty stack
            const bool again = button == 1 && same_item(at(slot), out);
            cx.fx.push_back(Effect::drop(std::move(out), true, true));
            if (!again) break;
        }
        return true;
    }

    if (input == C::kPickupAll && slot >= 0) {  // double click: gather the same item onto the cursor
        if (carried.empty() || (!at(slot).empty() && may_pickup(slot, cx))) return true;
        const i32 start = button == 0 ? 0 : size - 1, step = button == 0 ? 1 : -1;
        for (int pass = 0; pass < 2; ++pass) {  // partial stacks first, full ones after
            for (i32 i = start; i >= 0 && i < size && carried.count() < carried.max_stack(); i += step) {
                const ItemStack& target = at(i);
                if (target.empty() || !quick_replace(i, carried) || !may_pickup(i, cx) || i == menu_slot::kResult) continue;
                if (pass == 0 && target.count() == target.max_stack()) continue;
                const u32 all = target.count(), room = carried.max_stack() - carried.count();
                carried.grow(safe_take(i, all, room, cx).count());
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------- other packets

void Menu::set_creative(i32 slot, ItemStack stack, const Ctx& cx) {
    remote_is(slot, stack);
    set_by_player(slot, std::move(stack), cx);
}

void Menu::close(const Ctx& cx) {
    // AbstractContainerMenu.removed, then InventoryMenu's own: dropOrPlaceInInventory for the
    // cursor and for each cell of the crafting grid.
    if (!carried.empty()) inv.place_back(carried.take(), cx.creative, cx.fx);
    result_ = {};
    for (ItemStack& cell : craft) inv.place_back(cell.take(), cx.creative, cx.fx);
}

ItemStack Menu::take_from_hand(bool all) {
    ItemStack& hand = inv.selected_item();
    ItemStack out = hand.split(all ? hand.count() : 1);
    remote_is(slot_of(inv.selected), hand);  // the client took it out of its own copy before it asked
    return out;
}

void Menu::swap_hands(const Ctx& cx) {
    // setItemInHand twice: LivingEntity.setItemSlot runs the equip hook for each hand it fills.
    ItemStack off = std::move(inv.items[inv::kOffhand]);
    ItemStack main = std::move(inv.selected_item());
    equip_hook(equip::kOffhand, off, main, cx);
    equip_hook(equip::kMainhand, main, off, cx);
    inv.items[inv::kOffhand] = std::move(main);
    inv.selected_item() = std::move(off);
}

void Menu::pick(const ItemStack& stack, bool creative) {
    if (const i32 slot = inv.find_matching(stack); slot != -1) {
        if (slot < static_cast<i32>(inv::kHotbarSize))
            inv.selected = static_cast<u8>(slot);
        else
            inv.pick_slot(slot);
    } else if (creative) {
        inv.add_and_pick(stack);
    }
}

// ---------------------------------------------------------------------------- synchronisation

void Menu::remote_is(i32 slot, const ItemStack& stack) {
    remote_[slot] = stack;
    remote_known_ |= 1ull << slot;
}
void Menu::remote_is_unknown(i32 slot) {
    remote_[slot] = {};
    remote_known_ &= ~(1ull << slot);
}
void Menu::remote_carried_is(const ItemStack& stack) {
    remote_carried_ = stack;
    remote_known_ |= kCarriedBit;
}
void Menu::remote_carried_is_unknown() {
    remote_carried_ = {};
    remote_known_ &= ~kCarriedBit;
}

void Menu::send_all(base::ByteBuf& out, i32 threshold) {
    ItemView items[menu_slot::kCount];
    for (i32 slot = 0; slot < menu_slot::kCount; ++slot) {
        items[slot] = item::view(at(slot));
        remote_is(slot, at(slot));
    }
    remote_carried_is(carried);
    write_packet(out, threshold, cb::ContainerSetContent{kContainerId, next_state_id(), items, item::view(carried)});
}

void Menu::send_changes(base::ByteBuf& out, i32 threshold) {
    for (i32 slot = 0; slot < menu_slot::kCount; ++slot) {
        const ItemStack& now = at(slot);
        if (LIKELY((remote_known_ >> slot) & 1) && LIKELY(matches(remote_[slot], now))) continue;
        remote_is(slot, now);
        write_packet(out, threshold,
                     cb::ContainerSetSlot{kContainerId, next_state_id(), static_cast<i16>(slot), item::view(now)});
    }
    if ((remote_known_ & kCarriedBit) && matches(remote_carried_, carried)) return;
    remote_carried_is(carried);
    write_packet(out, threshold, cb::SetCursorItem{item::view(carried)});
}

}  // namespace server
