// SPDX-License-Identifier: GPL-3.0-or-later
// Recipes: what a crafting grid makes, what a furnace, a smoker and a blast furnace cook, what burns.
//
// The recipes are the vanilla data pack's (tools/mcgen reduces data/minecraft/recipe to tables);
// the rules are vanilla's (26.3): RecipeManager.getRecipeFor - the first recipe, in id order, that
// matches -, CraftingInput (the grid without its empty border), ShapedRecipePattern.matches (as
// laid out, or mirrored), ShapelessRecipe.matches (every ingredient gets a cell of its own),
// TransmuteRecipe (one input, some material; the result keeps the input's components).
//
// Not here: the recipe kinds that compute their result from components - dyed armour, banner
// copies, book copies, fireworks, map extending, repairing, shield decoration, tipped arrows,
// decorated pots (crafting_special_* and friends: 31 of the 1266 crafting recipes). A grid that
// holds one of those makes nothing. And everything that is not crafting or a furnace:
// stonecutting, smithing, brewing, what a campfire cooks.
//
// Lookup cost is what matters: a grid is matched on every change of it (a click), a furnace asks
// for its recipe every tick it burns. So: cooking is one table load per item; crafting walks
// only the recipes that name the grid's first item at all, a few dozen at worst (bench "craft").
//
// Tick thread only (results with components hold patches of item::patches()).
#pragma once

#include "item/stack.hpp"

namespace item {

// The RecipeType of an AbstractCookingRecipe, which is also what tells the three furnaces apart
// (AbstractFurnaceBlockEntity's recipeType): a furnace smelts, a smoker smokes, a blast furnace
// blasts. The values are the generated tables' (recipe::Cooker).
enum class Cooker : u8 { kSmelting, kSmoking, kBlasting };
// #fast_cooking (data/minecraft/predicate/block/fast_cooking): a smoker and a blast furnace burn
// a fuel in half the time and cook twice as fast with it (mcgen holds every fuel to that).
constexpr bool fast_cooking(Cooker c) { return c != Cooker::kSmelting; }

// A cooking recipe (SmeltingRecipe, SmokingRecipe, BlastingRecipe): what one `item` turns into.
struct Smelting {
    u16 result = 0;  // item id; 0 = the item does not cook there
    u8 count = 0;
    u16 ticks = 0;  // AbstractCookingRecipe.cookingTime
    float experience = 0;
};

// RecipeManager.getRecipeFor(type, new SingleRecipeInput(stack)): by item alone - no cooking
// ingredient looks at components.
Smelting cooking(Cooker type, u32 item);
inline Smelting smelting(u32 item) { return cooking(Cooker::kSmelting, item); }
// RecipePropertySet.FURNACE_INPUT / SMOKER_INPUT / BLAST_FURNACE_INPUT: AbstractFurnaceMenu.canSmelt.
inline bool cookable(Cooker type, const ItemStack& s) { return cooking(type, s.item()).result != 0; }
inline bool smeltable(const ItemStack& s) { return cookable(Cooker::kSmelting, s); }

// ItemStack.has(DataComponents.COOKING_FUEL), and AbstractFurnaceBlockEntity.getBurnDuration: 0
// for what does not burn; `fast`: in a smoker or a blast furnace. (A patch cannot set the
// component - it is not among those the server reads - only remove it.)
u32 fuel_ticks(const ItemStack& s, bool fast = false);
inline bool is_fuel(const ItemStack& s) { return fuel_ticks(s) != 0; }

// Item.getCraftingRemainder(): the item left behind when one of `item` is crafted with or burnt
// (a bucket for a lava bucket); 0 = nothing.
inline u32 crafting_remainder(u32 item) { return world::item_info(item).remainder; }

// RecipeManager.getRecipeFor(RecipeType.CRAFTING, CraftingInput.of(width, height, grid)) and
// Recipe.assemble: what the grid - `width` x `height` cells, row by row - makes. Empty when no
// recipe matches.
ItemStack craft(const ItemStack* grid, u32 width, u32 height);

}  // namespace item
