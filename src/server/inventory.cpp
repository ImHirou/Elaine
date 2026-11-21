// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/inventory.hpp"

#include <bit>

#include "item/recipes.hpp"
#include "proto/frame.hpp"
#include "proto/packets.hpp"

#include PROTO_GEN_ITEMS_HPP

namespace server {

using namespace proto;
namespace equip = item::equip;
using item::matches;
using item::same_item;
using item::same_item_same_components;

namespace {

constexpr u32 kNoLimit = 0x7FFFFFFF;  // Integer.MAX_VALUE where vanilla passes "as many as there are"
constexpr u32 kContainerLimit = 99;   // Container.getMaxStackSize()

constexpr i32 kInventorySlots = 36;  // addStandardInventorySlots: 27 + 9, the tail of every menu

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

i32 Menu::inventory_base() const {
    switch (kind_) {
        case Kind::kInventory: return menu_slot::kInvFirst;
        case Kind::kChest: return static_cast<i32>(rows_) * 9;
        case Kind::kFurnace: return static_cast<i32>(Furnace::kSlots);
        default: return 10;  // CraftingMenu.INV_SLOT_START
    }
}

i32 Menu::slot_count() const { return kind_ == Kind::kInventory ? menu_slot::kCount : inventory_base() + kInventorySlots; }

Menu::SlotKind Menu::kind_of(i32 slot) const {
    if (kind_ == Kind::kInventory) {
        if (slot == menu_slot::kResult) return SlotKind::kResult;
        if (slot < menu_slot::kCraftEnd) return SlotKind::kCraft;
        if (slot < menu_slot::kArmorEnd) return SlotKind::kArmor;
        return slot == menu_slot::kOffhand ? SlotKind::kOffhand : SlotKind::kPlain;
    }
    if (slot >= inventory_base() || kind_ == Kind::kChest) return SlotKind::kPlain;
    if (kind_ == Kind::kFurnace)
        return slot == static_cast<i32>(Furnace::kInput)  ? SlotKind::kInput
               : slot == static_cast<i32>(Furnace::kFuel) ? SlotKind::kFuel
                                                          : SlotKind::kOutput;
    return slot == 0 ? SlotKind::kResult : SlotKind::kCraft;
}

ItemStack& Menu::at(i32 slot) {
    DCHECK(slot >= 0 && slot < slot_count());
    if (kind_ == Kind::kInventory) {
        if (slot >= menu_slot::kHotbarFirst)
            return inv.items[slot == menu_slot::kOffhand ? inv::kOffhand : static_cast<u32>(slot - menu_slot::kHotbarFirst)];
        if (slot >= menu_slot::kInvFirst) return inv.items[slot];
        if (slot >= menu_slot::kArmorFirst)
            return inv.items[inv::kHead - static_cast<u32>(slot - menu_slot::kArmorFirst)];  // 5..8 = 39..36
        return slot == menu_slot::kResult ? result_ : craft[slot - menu_slot::kCraftFirst];
    }
    if (const i32 k = slot - inventory_base(); k >= 0)  // the inventory's 27, then the hotbar
        return inv.items[k < 27 ? 9 + k : k - 27];
    switch (kind_) {
        case Kind::kChest: return c_.chest[static_cast<u32>(slot) / Chest::kSlots][static_cast<u32>(slot) % Chest::kSlots];
        case Kind::kFurnace: return c_.furnace->items[slot];
        default: return slot == 0 ? table_result_ : table_[slot - 1];
    }
}

i32 Menu::slot_of(u32 index) {
    if (index < inv::kHotbarSize) return menu_slot::kHotbarFirst + static_cast<i32>(index);
    if (index < inv::kItems) return static_cast<i32>(index);
    if (index <= inv::kHead) return menu_slot::kArmorFirst + static_cast<i32>(inv::kHead - index);
    return index == inv::kOffhand ? menu_slot::kOffhand : -1;
}

u32 Menu::slot_limit(i32 slot) const { return kind_of(slot) == SlotKind::kArmor ? 1 : kContainerLimit; }

u32 Menu::max_for(i32 slot, const ItemStack& s) const {
    // FurnaceFuelSlot: one empty bucket at a time (it is there to be filled by a wet sponge).
    if (kind_of(slot) == SlotKind::kFuel && s.item() == gen::item::kBucket) return 1;
    return std::min(slot_limit(slot), s.max_stack());
}

bool Menu::may_place(i32 slot, const ItemStack& s) const {
    switch (kind_of(slot)) {
        case SlotKind::kResult:
        case SlotKind::kOutput: return false;
        // LivingEntity.isEquippableInSlot: the item's EQUIPPABLE names this slot and admits players.
        case SlotKind::kArmor: return s.equip_slot() == armor_slot(slot) && s.players_may_wear();
        case SlotKind::kFuel: return item::is_fuel(s) || s.item() == gen::item::kBucket;  // FurnaceFuelSlot
        default: return true;
    }
}

bool Menu::may_pickup(i32 slot, const Ctx& cx) const {
    if (kind_of(slot) != SlotKind::kArmor || cx.creative) return true;
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

// Slot.set -> Container.setItem. Two containers do more than store the stack: a crafting grid
// tells its menu (TransientCraftingContainer), a furnace starts cooking anew.
void Menu::set(i32 slot, ItemStack s, const Ctx& cx) {
    switch (kind_of(slot)) {
        case SlotKind::kCraft:
            at(slot) = std::move(s);
            grid_changed(cx);
            break;
        case SlotKind::kInput:
        case SlotKind::kFuel:
        case SlotKind::kOutput: c_.furnace->set_item(static_cast<u32>(slot), std::move(s)); break;
        default: at(slot) = std::move(s);
    }
}

void Menu::set_by_player(i32 slot, ItemStack s, const Ctx& cx) {
    if (const SlotKind kind = kind_of(slot); kind == SlotKind::kArmor)
        equip_hook(armor_slot(slot), at(slot), s, cx);
    else if (kind == SlotKind::kOffhand)
        equip_hook(equip::kOffhand, at(slot), s, cx);
    set(slot, std::move(s), cx);
}

// Slot.remove -> Container.removeItem
ItemStack Menu::remove(i32 slot, u32 amount, const Ctx& cx) {
    ItemStack& in = at(slot);
    const SlotKind kind = kind_of(slot);
    if (kind == SlotKind::kResult) return in.take();  // ResultContainer.removeItem: all of it, whatever was asked
    ItemStack taken = in.split(amount);
    if (kind == SlotKind::kCraft && !taken.empty()) grid_changed(cx);  // TransientCraftingContainer.removeItem
    return taken;
}

bool Menu::try_remove(i32 slot, u32 amount, u32 max_amount, const Ctx& cx, ItemStack& taken) {
    if (!may_pickup(slot, cx)) return false;
    // allowModification: a slot that cannot take its own stack back is all or nothing.
    if (!(may_pickup(slot, cx) && may_place(slot, at(slot))) && max_amount < at(slot).count()) return false;
    taken = remove(slot, std::min(amount, max_amount), cx);
    if (taken.empty()) return false;
    if (at(slot).empty()) {  // setByPlayer(EMPTY, taken): what left the slot is what was "taken off"
        if (const SlotKind kind = kind_of(slot); kind == SlotKind::kArmor)
            equip_hook(armor_slot(slot), taken, {}, cx);
        else if (kind == SlotKind::kOffhand)
            equip_hook(equip::kOffhand, taken, {}, cx);
        set(slot, {}, cx);
    }
    return true;
}

ItemStack Menu::safe_take(i32 slot, u32 amount, u32 max_amount, const Ctx& cx) {
    ItemStack taken;
    if (!try_remove(slot, amount, max_amount, cx, taken)) return {};
    on_take(slot, cx);
    return taken;
}

// CraftingMenu.slotChangedCraftingGrid (InventoryMenu.slotsChanged calls the same): what the
// grid makes now goes into the result slot, and to the client at once - not with the next
// synchronisation, and once per change of the grid: taking a result off three ingredients is
// three of these.
void Menu::grid_changed(const Ctx& cx) {
    const bool table = kind_ == Kind::kCrafting;
    ItemStack made = table ? item::craft(table_, 3, 3) : item::craft(craft, 2, 2);
    (table ? table_result_ : result_) = made;
    remote_is(0, made);
    cx.fx.push_back(Effect::set_slot(container_id(), next_state_id(), std::move(made)));
}

// Slot.onTake. Only a crafting result does anything (ResultSlot.onTake): one of every
// ingredient is used up, and what an ingredient leaves behind (a bucket) takes its place - or
// goes to the inventory when the cell still holds something else.
//
// A furnace's result would hand out the experience its recipes have earned here
// (FurnaceResultSlot.checkTakeAchievements); there are no experience orbs yet.
void Menu::on_take(i32 slot, const Ctx& cx) {
    if (kind_of(slot) != SlotKind::kResult) return;
    const bool table = kind_ == Kind::kCrafting;
    ItemStack* grid = table ? table_ : craft;
    const u32 n = table ? 9 : 4;
    // getRemainingItems(input) is asked before anything is removed.
    u32 remainder[9];
    for (u32 k = 0; k < n; ++k) remainder[k] = grid[k].empty() ? 0 : item::crafting_remainder(grid[k].item());
    // In the order of the positioned input: row by row. (Cells outside it are empty.)
    for (u32 k = 0; k < n; ++k) {
        ItemStack& cell = grid[k];
        if (!cell.empty()) {  // craftSlots.removeItem(slot, 1)
            cell.shrink(1);
            if (cell.empty()) cell = {};
            grid_changed(cx);
        }
        if (!remainder[k]) continue;
        ItemStack left(remainder[k], 1);
        if (cell.empty()) {
            cell = std::move(left);
            grid_changed(cx);
        } else if (same_item_same_components(cell, left)) {
            cell.grow(1);
            grid_changed(cx);
        } else if (!inv.add(-1, left, cx.creative)) {
            cx.fx.push_back(Effect::drop(std::move(left), false, true));
        }
    }
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

// quickMoveStack of InventoryMenu, ChestMenu, AbstractFurnaceMenu and CraftingMenu: where a
// shift-click sends the stack of a slot. What it returns only tells the caller whether to try
// again (a crafting result refills).
ItemStack Menu::quick_move(i32 slot, const Ctx& cx) {
    using namespace menu_slot;
    ItemStack& stack = at(slot);
    if (stack.empty()) return {};
    const ItemStack clicked = stack;
    const i32 base = inventory_base(), hotbar = base + 27, end = base + kInventorySlots;
    bool moved;
    switch (kind_) {
        case Kind::kInventory: {
            // LivingEntity.getEquipmentSlotForItem: where its EQUIPPABLE says it goes, the main hand otherwise.
            const u32 wear = clicked.equip_slot() == item::kNoEquipSlot ? equip::kMainhand : clicked.equip_slot();
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
            break;
        }
        case Kind::kChest:  // between the chest and the inventory
            moved = slot < base ? move_to(stack, base, end, true, cx) : move_to(stack, 0, base, false, cx);
            break;
        case Kind::kFurnace:
            if (slot == static_cast<i32>(Furnace::kResult))
                moved = move_to(stack, base, end, true, cx);
            else if (slot < base)
                moved = move_to(stack, base, end, false, cx);
            else if (item::smeltable(stack))  // canSmelt: RecipePropertySet.FURNACE_INPUT
                moved = move_to(stack, 0, 1, false, cx);
            else if (item::is_fuel(stack))
                moved = move_to(stack, 1, 2, false, cx);
            else if (slot < hotbar)
                moved = move_to(stack, hotbar, end, false, cx);
            else
                moved = move_to(stack, base, hotbar, false, cx);
            break;
        default:  // CraftingMenu
            if (slot == 0)
                moved = move_to(stack, base, end, true, cx);
            else if (slot < base)
                moved = move_to(stack, base, end, false, cx);
            else  // into the grid first, then to the other half of the inventory
                moved = move_to(stack, 1, base, false, cx) ||
                        (slot < hotbar ? move_to(stack, hotbar, end, false, cx) : move_to(stack, base, hotbar, false, cx));
            break;
    }
    if (!moved) return {};
    if (at(slot).empty()) {  // setByPlayer(EMPTY, clicked)
        if (const SlotKind kind = kind_of(slot); kind == SlotKind::kArmor)
            equip_hook(armor_slot(slot), clicked, {}, cx);
        else if (kind == SlotKind::kOffhand)
            equip_hook(equip::kOffhand, clicked, {}, cx);
        set(slot, {}, cx);
    }
    if (kind_ == Kind::kChest) return clicked;
    if (at(slot).count() == clicked.count()) return {};
    // slot.onTake(player, stack). For a crafting result that uses up the ingredients and puts
    // the next result into the slot; what the inventory had no room for is thrown out.
    const bool result = kind_of(slot) == SlotKind::kResult;
    ItemStack rest = result ? at(slot).take() : ItemStack();
    on_take(slot, cx);
    if (result && !rest.empty()) cx.fx.push_back(Effect::drop(std::move(rest), false, true));
    return clicked;
}

bool Menu::click(i32 slot, i32 button, u32 input, const Ctx& cx) {
    using C = sb::ContainerClick;
    c_ = cx.c;
    const i32 size = slot_count();
    DCHECK(slot < size);  // handleContainerClick: isValidSlotIndex

    if (input == C::kQuickCraft) {
        Sync& q = sync();
        const u8 expected = q.quickcraft_status;
        q.quickcraft_status = static_cast<u8>(button & 3);  // getQuickcraftHeader: 0 start, 1 add a slot, 2 end
        const u32 n = static_cast<u32>(std::popcount(q.quickcraft_slots[0]) + std::popcount(q.quickcraft_slots[1]));
        if ((expected != 1 || q.quickcraft_status != 2) && expected != q.quickcraft_status) {
            reset_quickcraft();
        } else if (carried.empty()) {
            reset_quickcraft();
        } else if (q.quickcraft_status == 0) {
            q.quickcraft_type = static_cast<i8>((button >> 2) & 3);  // 0 spread evenly, 1 one each, 2 a full stack each
            if (q.quickcraft_type == 0 || q.quickcraft_type == 1 || (q.quickcraft_type == 2 && cx.creative)) {
                q.quickcraft_status = 1;
                q.quickcraft_slots[0] = q.quickcraft_slots[1] = 0;
            } else {
                reset_quickcraft();
            }
        } else if (q.quickcraft_status == 1) {
            if (slot < 0) return false;  // vanilla indexes its slot list with it
            if (quick_replace(slot, carried) && may_place(slot, carried) && (q.quickcraft_type == 2 || carried.count() > n))
                q.quickcraft_slots[slot >> 6] |= 1ull << (slot & 63);
        } else if (q.quickcraft_status == 2) {
            if (n == 1) {  // a drag over one slot is a click on it
                const i32 only = q.quickcraft_slots[0] ? std::countr_zero(q.quickcraft_slots[0])
                                                       : 64 + std::countr_zero(q.quickcraft_slots[1]);
                const i32 type = q.quickcraft_type;
                reset_quickcraft();
                return click(only, type, C::kPickup, cx);
            }
            if (n > 1) {
                ItemStack source = carried;
                if (source.empty()) {
                    reset_quickcraft();
                    return true;
                }
                i32 remaining = static_cast<i32>(carried.count());
                for (i32 to = 0; to < size; ++to) {
                    if (!((q.quickcraft_slots[to >> 6] >> (to & 63)) & 1)) continue;
                    if (!(quick_replace(to, carried) && may_place(to, carried) &&
                          (q.quickcraft_type == 2 || carried.count() >= n)))
                        continue;
                    const u32 held = at(to).count();
                    // getQuickCraftPlaceCount
                    const u32 each = q.quickcraft_type == 0   ? source.count() / n
                                     : q.quickcraft_type == 1 ? 1
                                                              : source.max_stack();
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
    if (sync().quickcraft_status != 0) {
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
                    if (try_remove(slot, amount, kNoLimit, cx, taken)) {
                        carried = std::move(taken);
                        on_take(slot, cx);
                    }
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
                    if (try_remove(slot, amount, room, cx, taken)) {
                        carried.grow(taken.count());
                        on_take(slot, cx);
                    }
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
            on_take(slot, cx);
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
                on_take(slot, cx);
                if (!inv.add(-1, was, cx.creative)) cx.fx.push_back(Effect::drop(std::move(was), true, true));
            } else {
                set_by_player(slot, source.take(), cx);
                source = std::move(was);
                on_take(slot, cx);
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
                // canTakeItemForPickAll: not out of a crafting result.
                if (target.empty() || !quick_replace(i, carried) || !may_pickup(i, cx) || kind_of(i) == SlotKind::kResult)
                    continue;
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
    // The player's own menu, whichever is open: its slots, its state id, its cursor.
    const Kind was = kind_;
    kind_ = Kind::kInventory;
    remote_is(slot, stack);
    set_by_player(slot, std::move(stack), cx);
    kind_ = was;
}

void Menu::open(Kind kind, u32 container_id, u32 rows) {
    DCHECK(kind_ == Kind::kInventory && kind != Kind::kInventory);
    own_carried_ = std::move(carried);  // each menu has a cursor of its own
    carried = {};
    kind_ = kind;
    container_id_ = static_cast<u8>(container_id);
    rows_ = static_cast<u8>(rows);
    open_ = Sync{};
}

void Menu::close(const Ctx& cx) {
    c_ = cx.c;
    // AbstractContainerMenu.removed: dropOrPlaceInInventory for the cursor.
    if (!carried.empty()) inv.place_back(carried.take(), cx.creative, cx.fx);
    if (kind_ == Kind::kInventory) {  // InventoryMenu.removed: the crafting grid too
        result_ = {};
        for (ItemStack& cell : craft) inv.place_back(cell.take(), cx.creative, cx.fx);
        return;
    }
    if (kind_ == Kind::kCrafting) {  // CraftingMenu.removed: clearContainer(craftSlots)
        for (ItemStack& cell : table_) inv.place_back(cell.take(), cx.creative, cx.fx);
        table_result_ = {};
    }
    // inventoryMenu.transferState(containerMenu): both menus show the inventory's 36 slots, and
    // what the client holds there is what the closing menu knew.
    const i32 base = inventory_base();
    for (i32 k = 0; k < kInventorySlots; ++k) {
        const i32 from = base + k, to = menu_slot::kInvFirst + k;
        const bool known = (open_.known[from >> 6] >> (from & 63)) & 1;
        own_.remote[to] = known ? open_.remote[from] : ItemStack();
        own_.known[0] = (own_.known[0] & ~(1ull << to)) | (static_cast<u64>(known) << to);
    }
    kind_ = Kind::kInventory;
    carried = std::move(own_carried_);
    own_carried_ = {};
    open_ = Sync{};
}

ItemStack Menu::take_from_hand(bool all) {
    ItemStack& hand = inv.selected_item();
    ItemStack out = hand.split(all ? hand.count() : 1);
    // containerMenu.findSlot(inventory, selected): the client took it out of its own copy before it asked.
    remote_is(kind_ == Kind::kInventory ? slot_of(inv.selected) : inventory_base() + 27 + inv.selected, hand);
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
    sync().remote[slot] = stack;
    sync().known[slot >> 6] |= 1ull << (slot & 63);
}
void Menu::remote_is_unknown(i32 slot) {
    sync().remote[slot] = {};
    sync().known[slot >> 6] &= ~(1ull << (slot & 63));
}
void Menu::remote_carried_is(const ItemStack& stack) {
    sync().remote_carried = stack;
    sync().carried_known = true;
}
void Menu::remote_carried_is_unknown() {
    sync().remote_carried = {};
    sync().carried_known = false;
}
void Menu::own_remote_is_unknown(i32 slot) {
    own_.remote[slot] = {};
    own_.known[0] &= ~(1ull << slot);
}

void Menu::send_all(base::ByteBuf& out, i32 threshold, const Containers& c) {
    c_ = c;
    const i32 n = slot_count();
    ItemView items[menu_slot::kMax];
    for (i32 slot = 0; slot < n; ++slot) {
        items[slot] = item::view(at(slot));
        remote_is(slot, at(slot));
    }
    remote_carried_is(carried);
    write_packet(out, threshold,
                 cb::ContainerSetContent{container_id(), next_state_id(), {items, static_cast<usize>(n)}, item::view(carried)});
    if (kind_ != Kind::kFurnace) return;
    for (u32 i = 0; i < Furnace::kDataCount; ++i) {  // sendInitialData: every data slot after the content
        open_.remote_data[i] = c.furnace->data(i);
        write_packet(out, threshold,
                     cb::ContainerSetData{container_id(), static_cast<i16>(i), static_cast<i16>(open_.remote_data[i])});
    }
}

void Menu::send_changes(base::ByteBuf& out, i32 threshold, const Containers& c) {
    c_ = c;
    Sync& s = sync();
    const i32 n = slot_count();
    for (i32 slot = 0; slot < n; ++slot) {
        const ItemStack& now = at(slot);
        if (LIKELY((s.known[slot >> 6] >> (slot & 63)) & 1) && LIKELY(matches(s.remote[slot], now))) continue;
        remote_is(slot, now);
        write_packet(out, threshold,
                     cb::ContainerSetSlot{container_id(), next_state_id(), static_cast<i16>(slot), item::view(now)});
    }
    if (!(s.carried_known && matches(s.remote_carried, carried))) {
        remote_carried_is(carried);
        write_packet(out, threshold, cb::SetCursorItem{item::view(carried)});
    }
    if (kind_ != Kind::kFurnace) return;
    for (u32 i = 0; i < Furnace::kDataCount; ++i) {  // synchronizeDataSlotToRemote
        const i32 now = c.furnace->data(i);
        if (now == s.remote_data[i]) continue;
        s.remote_data[i] = now;
        write_packet(out, threshold, cb::ContainerSetData{container_id(), static_cast<i16>(i), static_cast<i16>(now)});
    }
}

void Menu::send_own_changes(base::ByteBuf& out, i32 threshold) {
    const Kind was = kind_;
    if (was != Kind::kInventory) std::swap(carried, own_carried_);
    kind_ = Kind::kInventory;
    send_changes(out, threshold);
    kind_ = was;
    if (was != Kind::kInventory) std::swap(carried, own_carried_);
}

}  // namespace server
