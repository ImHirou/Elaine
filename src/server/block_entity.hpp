// SPDX-License-Identifier: GPL-3.0-or-later
// What block entities hold: a chest's 27 stacks (a barrel's too), a furnace's three and its
// timers - of the furnace, the smoker and the blast furnace, which are one block entity that
// cooks by three sets of recipes.
//
// Plain data and functions over it - no world, no packets - so that a furnace runs in a unit
// test and in a benchmark. Where it is in the world, who has it open and what its ticking does
// to the block around it is the game's business (server/block_entities.cpp); where a block
// entity is, and of which type, is the dimension's (world::BlockEntityRef).
//
// The rules are vanilla's (26.3): ChestBlockEntity, BarrelBlockEntity, AbstractFurnaceBlockEntity
// (serverTick, setItem, canBurn, burn, consumeFuel, getTotalCookTime, getSpeedMultiplier).
//
// Not here: custom names and locks (a chest named on an anvil; BaseContainerBlockEntity.name /
// lockKey), loot tables (RandomizableContainerBlockEntity), the experience a furnace has
// stored up (recipesUsed) - it gives none when emptied, and experience orbs do not exist yet.
#pragma once

#include "item/recipes.hpp"
#include "item/stack.hpp"

namespace server {

using item::ItemStack;

// A chest of any kind, and a barrel: 27 stacks and who has them open.
struct Chest {
    static constexpr u32 kSlots = 27;
    ItemStack items[kSlots];
    i32 x = 0, y = 0, z = 0;
    u16 open = 0;        // ContainerOpenersCounter.openCount: players that have it open
    bool alive = false;  // false: a free entry of the store
    // What its saved tag held that nothing here reads and nothing here needs to act as vanilla
    // does (an imported block entity's `components`...): an entry of Game::opaque_, ~0 for none.
    u32 keep = ~0u;
};

struct Furnace {
    static constexpr u32 kInput = 0, kFuel = 1, kResult = 2, kSlots = 3;
    static constexpr u32 kDataCount = 4;  // AbstractFurnaceMenu.DATA_COUNT

    // What one tick did beyond the furnace itself.
    struct Ticked {
        bool lit_changed = false;  // the block's `lit` property has to follow
        bool lit = false;
        ItemStack spilled;  // consumeFuel: the remainder of a fuel that left more fuel behind it drops out
    };

    ItemStack items[kSlots];
    // AbstractFurnaceBlockEntity.dataAccess, in its order: a menu shows these four.
    i32 lit_remaining = 0;  // litTimeRemaining: ticks the fuel in the fire still burns
    i32 lit_total = 0;      // litTotalTime: what that fuel burned for in all
    i32 cook = 0;           // cookingTimer: ticks the input has been cooking
    i32 cook_total = 0;     // cookingTotalTime: ticks it takes
    // speedMultiplier: of the fuel that was lit last - 1 in a furnace, 2 in a smoker or a blast
    // furnace, and 1 in any of them until a fuel has been lit. The cooking time is divided by it.
    float speed = 1.0f;
    item::Cooker type = item::Cooker::kSmelting;  // recipeType: which of the three it is
    i32 x = 0, y = 0, z = 0;
    bool alive = false;
    u32 keep = ~0u;  // as Chest::keep: the recipes it has used (experience it owes), for one

    i32 data(u32 i) const { return i == 0 ? lit_remaining : i == 1 ? lit_total : i == 2 ? cook : cook_total; }
    // Nothing a tick would change: not burning, nothing cooked half-way, and not both an input
    // and a fuel to start with. The test a bulk tick asks first.
    bool idle() const { return lit_remaining <= 0 && cook <= 0 && (items[kInput].empty() || items[kFuel].empty()); }

    // AbstractFurnaceBlockEntity.setItem: another kind of input starts cooking from nothing.
    void set_item(u32 slot, ItemStack stack);
    // AbstractFurnaceBlockEntity.serverTick
    Ticked tick();
};

}  // namespace server
