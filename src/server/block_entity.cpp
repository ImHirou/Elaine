// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/block_entity.hpp"

#include <cmath>

#include PROTO_GEN_ITEMS_HPP

namespace server {

namespace {

constexpr i32 kBurnTimeStandard = 200;  // AbstractFurnaceBlockEntity.BURN_TIME_STANDARD
constexpr i32 kCoolSpeed = 2;           // BURN_COOL_SPEED
constexpr u32 kContainerLimit = 99;     // Container.getMaxStackSize()

// getTotalCookTime(recipe, entity): the recipe's cooking time, divided by the speed of the fuel
// that was lit last - in float, rounded up, as vanilla has it.
i32 total_cook_time(i32 ticks, float speed) {
    return speed > 0.0f ? static_cast<i32>(std::ceil(static_cast<double>(static_cast<float>(ticks) / speed))) : ticks;
}

}  // namespace

void Furnace::set_item(u32 slot, ItemStack stack) {
    const bool same = !stack.empty() && item::same_item_same_components(items[slot], stack);
    items[slot] = std::move(stack);
    if (slot == kInput && !same) {
        // getTotalCookTime(level, entity): of the recipe for what is in the input slot now, 200 without one.
        const item::Smelting r = item::cooking(type, items[kInput].item());
        cook_total = r.result ? total_cook_time(r.ticks, speed) : kBurnTimeStandard;
        cook = 0;
    }
}

Furnace::Ticked Furnace::tick() {
    Ticked out;
    const bool was_lit = lit_remaining > 0;
    bool lit = false;
    if (was_lit) lit = --lit_remaining > 0;

    ItemStack& fuel = items[kFuel];
    ItemStack& input = items[kInput];
    const bool has_input = !input.empty(), has_fuel = !fuel.empty();
    if (lit || (has_fuel && has_input)) {
        if (!has_input) {
            cook = 0;
        } else if (const item::Smelting recipe = item::cooking(type, input.item()); recipe.result) {
            const ItemStack made(recipe.result, recipe.count);  // recipe.assemble(input)
            // canBurn: the result slot is empty, or holds the same thing with room for more.
            const ItemStack& result = items[kResult];
            const bool can_burn =
                result.empty() || (item::same_item_same_components(result, made) &&
                                   result.count() + made.count() <= std::min(kContainerLimit, made.max_stack()));
            if (!can_burn) {
                cook = 0;
            } else {
                if (!lit) {  // light the next piece of fuel
                    // getBurnDuration, getSpeedMultiplier: the fuel's, by the block it burns in
                    // (0 and 1 for what has no COOKING_FUEL: a bucket waiting for a wet sponge).
                    const bool fast = item::fast_cooking(type);
                    const i32 burns = static_cast<i32>(item::fuel_ticks(fuel, fast));
                    lit_remaining = lit_total = burns;
                    speed = burns > 0 && fast ? proto::gen::kFastCookingSpeed : 1.0f;
                    if (cook_total > 0 && cook < cook_total) {
                        const float done = static_cast<float>(cook) / static_cast<float>(cook_total);
                        cook_total = total_cook_time(recipe.ticks, speed);
                        cook = static_cast<i32>(std::ceil(static_cast<double>(done * static_cast<float>(cook_total))));
                    }
                    if (burns > 0) {
                        // consumeFuel: one off the stack; what it leaves (a lava bucket's bucket)
                        // takes the slot if that is now empty, and drops out of the furnace if not.
                        const u32 remainder = item::crafting_remainder(fuel.item());
                        fuel.shrink(1);
                        if (remainder) {
                            if (fuel.empty())
                                fuel = ItemStack(remainder, 1);
                            else
                                out.spilled = ItemStack(remainder, 1);
                        } else if (fuel.empty()) {
                            fuel = {};
                        }
                        lit = true;
                    }
                }
                if (lit) {
                    if (++cook >= cook_total) {
                        cook = 0;
                        cook_total = total_cook_time(recipe.ticks, speed);
                        // burn: one input becomes the result. A wet sponge dries into a bucket
                        // standing in the fuel slot.
                        if (items[kResult].empty())
                            items[kResult] = made;
                        else
                            items[kResult].grow(made.count());
                        if (input.item() == proto::gen::item::kWetSponge && items[kFuel].item() == proto::gen::item::kBucket)
                            items[kFuel] = ItemStack(proto::gen::item::kWaterBucket, 1);
                        input.shrink(1);
                        if (input.empty()) input = {};
                    }
                } else {
                    cook = 0;
                }
            }
        }
    } else if (cook > 0) {
        cook = std::clamp(cook - kCoolSpeed, 0, cook_total);
    }
    out.lit = lit;
    out.lit_changed = was_lit != lit;
    return out;
}

}  // namespace server
