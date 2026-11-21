// SPDX-License-Identifier: GPL-3.0-or-later
#include "item/recipes.hpp"

#include <cstdlib>
#include <vector>

#include PROTO_GEN_RECIPES_HPP

namespace item {

namespace rc = proto::gen::recipe;

namespace {

// Ingredient.test: is `item` one of the ingredient's items? The lists are sorted and short
// (one item for most, a few dozen for #planks or #logs).
bool accepts(u32 ingredient, u32 item) {
    const u16* lo = rc::kIngredientItems + rc::kIngredientFirst[ingredient];
    const u16* hi = rc::kIngredientItems + rc::kIngredientFirst[ingredient + 1];
    if (hi - lo == 1) return *lo == item;
    return std::binary_search(lo, hi, static_cast<u16>(item));
}

// Everything derived from the generated tables, once.
struct Index {
    // Per item: the crafting recipes that name it in some ingredient, ascending - the order
    // RecipeManager tries them in. recipes[first[item] .. first[item + 1]).
    std::vector<u32> first;
    std::vector<u16> recipes;
    std::vector<u16> smelt;             // per item: its smelting recipe + 1, 0 = none
    std::vector<PatchId> result_patch;  // rc::kPatchBytes, interned; held for the life of the process

    Index() {
        const u32 items = world::item_count();
        // Result components go through the same decoders as a client's (which is also what
        // checks the generator's encoding of them).
        for (u32 i = 0; i < rc::kPatchCount; ++i) {
            proto::Reader r(rc::kPatchBytes + rc::kPatchFirst[i], rc::kPatchFirst[i + 1] - rc::kPatchFirst[i]);
            PatchId id = 0;
            if (patches().read_untrusted(r, id) != Verdict::kOk || !r.done() || !id) std::abort();
            result_patch.push_back(id);
        }
        std::vector<std::vector<u16>> by_item(items);
        const auto name = [&](u32 recipe, u32 ingredient) {
            for (u32 k = rc::kIngredientFirst[ingredient]; k < rc::kIngredientFirst[ingredient + 1]; ++k) {
                auto& list = by_item[rc::kIngredientItems[k]];
                if (list.empty() || list.back() != recipe) list.push_back(static_cast<u16>(recipe));
            }
        };
        for (u32 i = 0; i < rc::kCraftingCount; ++i) {
            const rc::Crafting& c = rc::kCrafting[i];
            const u32 n = c.kind == rc::kShaped ? c.a * c.b : c.kind == rc::kShapeless ? c.a : 2u;
            for (u32 k = 0; k < n; ++k)
                if (c.ing[k] != rc::kNoIngredient) name(i, c.ing[k]);
        }
        first.assign(items + 1, 0);
        for (u32 item = 0; item < items; ++item) {
            first[item] = static_cast<u32>(recipes.size());
            recipes.insert(recipes.end(), by_item[item].begin(), by_item[item].end());
        }
        first[items] = static_cast<u32>(recipes.size());
        // The first smelting recipe, in id order, that takes the item.
        smelt.assign(items, 0);
        for (u32 i = rc::kSmeltingCount; i-- > 0;) {
            const u32 ing = rc::kSmelting[i].ingredient;
            for (u32 k = rc::kIngredientFirst[ing]; k < rc::kIngredientFirst[ing + 1]; ++k)
                smelt[rc::kIngredientItems[k]] = static_cast<u16>(i + 1);
        }
    }
    ~Index() {
        for (const PatchId id : result_patch) patches().release(id);
    }
};

const Index& index() {
    static const Index idx;
    return idx;
}

// CraftingInput: the grid's cells inside the bounding box of what is in it.
struct Input {
    const ItemStack* cell[9];
    u32 width = 0, height = 0, count = 0;  // ingredientCount: the cells that hold something
    u32 size() const { return width * height; }
};

bool matches_shaped(const rc::Crafting& c, const Input& in) {
    if (in.width != c.a || in.height != c.b) return false;
    u32 wanted = 0;
    for (u32 k = 0; k < in.size(); ++k) wanted += c.ing[k] != rc::kNoIngredient;
    if (in.count != wanted) return false;
    // Ingredient.testOptionalIngredient: an empty pattern cell wants an empty grid cell.
    const auto fits = [&](bool flip) {
        for (u32 y = 0; y < c.b; ++y) {
            for (u32 x = 0; x < c.a; ++x) {
                const u32 ing = c.ing[(flip ? c.a - x - 1 : x) + y * c.a];
                const ItemStack& s = *in.cell[x + y * c.a];
                if (ing == rc::kNoIngredient ? !s.empty() : !accepts(ing, s.item())) return false;
            }
        }
        return true;
    };
    return fits(true) || fits(false);
}

// StackedItemContents.canCraft: every ingredient needs a cell of its own. A matching in a graph
// of at most 9 x 9, by augmenting paths.
bool matches_shapeless(const rc::Crafting& c, const Input& in) {
    if (in.count != c.a) return false;
    if (in.size() == 1 && c.a == 1) return accepts(c.ing[0], in.cell[0]->item());
    u32 items[9], n = 0;
    for (u32 k = 0; k < in.size(); ++k)
        if (!in.cell[k]->empty()) items[n++] = in.cell[k]->item();
    i32 cell_of[9];  // ingredient -> the cell it took
    std::fill_n(cell_of, 9, -1);
    i32 ing_of[9];  // cell -> the ingredient that took it
    std::fill_n(ing_of, 9, -1);
    for (u32 start = 0; start < n; ++start) {
        // Breadth-first over ingredients for a free cell, then flip the path.
        i32 from_cell[9], queue[9];  // from_cell[cell]: the ingredient that reached it
        std::fill_n(from_cell, 9, -1);
        u32 head = 0, tail = 0;
        queue[tail++] = static_cast<i32>(start);
        i32 free_cell = -1;
        while (head < tail && free_cell < 0) {
            const i32 ing = queue[head++];
            for (u32 cell = 0; cell < n; ++cell) {
                if (from_cell[cell] >= 0 || !accepts(c.ing[ing], items[cell])) continue;
                from_cell[cell] = ing;
                if (ing_of[cell] < 0) {
                    free_cell = static_cast<i32>(cell);
                    break;
                }
                queue[tail++] = ing_of[cell];
            }
        }
        if (free_cell < 0) return false;
        for (i32 cell = free_cell; cell >= 0;) {
            const i32 ing = from_cell[cell], was = cell_of[ing];
            cell_of[ing] = cell;
            ing_of[cell] = ing;
            cell = was;
        }
    }
    return true;
}

// TransmuteRecipe.matches / assemble. `out` receives the result when it matches.
bool transmute(const rc::Crafting& c, const Input& in, ItemStack* out) {
    const u32 lo = c.a, hi = c.b;
    if (in.count < lo + 1 || in.count > hi + 1) return false;
    const ItemStack* found = nullptr;
    u32 material = 0;
    for (u32 k = 0; k < in.size(); ++k) {
        const ItemStack& s = *in.cell[k];
        if (s.empty()) continue;
        if (accepts(c.ing[0], s.item())) {
            if (found) return false;
            found = &s;
        } else if (accepts(c.ing[1], s.item())) {
            if (++material > hi) return false;
        } else {
            return false;
        }
    }
    if (!found || material < lo || material > hi) return false;
    // createWithOriginalComponents: the result's item (the input's own when the recipe names
    // none) with the input's component patch.
    const auto make = [&](u32 extra) {
        const u32 item = c.result ? c.result : found->item();
        if (!found->has_patch()) return ItemStack(item, c.count + extra);
        patches().retain(found->patch());
        return ItemStack(item, c.count + extra, found->patch());
    };
    const u32 size = c.extra ? material + c.count : c.count;
    if (size == 1) {  // a recipe that would hand back what went in is no recipe
        const ItemStack same = make(0);
        if (same.empty() || same_item_same_components(*found, same)) return false;
    }
    if (out) *out = make(c.extra ? material : 0);
    return true;
}

}  // namespace

Smelting smelting(u32 item) {
    const u16 at = index().smelt[item];
    if (!at) return {};
    const rc::Smelting& s = rc::kSmelting[at - 1];
    return {s.result, s.count, s.ticks, s.experience};
}

u32 fuel_ticks(const ItemStack& s) {
    if (s.empty()) return 0;
    if (UNLIKELY(s.has_patch()) && patches().find(s.patch(), component::kCookingFuel).state == Patches::State::kRemoved) return 0;
    return s.info().fuel_ticks;
}

ItemStack craft(const ItemStack* grid, u32 width, u32 height) {
    // CraftingInput.ofPositioned
    u32 left = width, right = 0, top = height, bottom = 0, count = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            if (grid[x + y * width].empty()) continue;
            left = std::min(left, x), right = std::max(right, x);
            top = std::min(top, y), bottom = std::max(bottom, y);
            ++count;
        }
    }
    if (!count) return {};  // RecipeMap.getRecipesFor: an empty input matches nothing
    Input in;
    in.width = right - left + 1;
    in.height = bottom - top + 1;
    in.count = count;
    const ItemStack* fewest = nullptr;
    const Index& idx = index();
    u32 fewest_n = ~0u;
    for (u32 y = 0; y < in.height; ++y) {
        for (u32 x = 0; x < in.width; ++x) {
            const ItemStack& s = grid[x + left + (y + top) * width];
            in.cell[x + y * in.width] = &s;
            if (s.empty()) continue;
            // Any recipe that matches names every item of the grid: walk the shortest list.
            const u32 n = idx.first[s.item() + 1] - idx.first[s.item()];
            if (n < fewest_n) fewest_n = n, fewest = &s;
        }
    }
    for (u32 k = idx.first[fewest->item()]; k < idx.first[fewest->item() + 1]; ++k) {
        const rc::Crafting& c = rc::kCrafting[idx.recipes[k]];
        ItemStack out;
        switch (c.kind) {
            case rc::kShaped:
                if (!matches_shaped(c, in)) continue;
                break;
            case rc::kShapeless:
                if (!matches_shapeless(c, in)) continue;
                break;
            default:
                if (!transmute(c, in, &out)) continue;
                return out;
        }
        if (!c.patch) return ItemStack(c.result, c.count);
        patches().retain(idx.result_patch[c.patch - 1]);
        return ItemStack(c.result, c.count, idx.result_patch[c.patch - 1]);
    }
    return {};
}

}  // namespace item
