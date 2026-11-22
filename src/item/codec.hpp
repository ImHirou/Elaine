// SPDX-License-Identifier: GPL-3.0-or-later
// Item stacks and their components as tags: what vanilla's Codecs read and write.
//
// A component has two forms. On the wire it is the bytes of its StreamCodec (components.hpp:
// what a patch is stored as). In a tag - a saved stack, the `[damage=3]` of a command, the
// SelectedItem of `/data get` - it is what its Codec makes of it: `damage` an int, `rarity` a
// name, `enchantments` a map of names to levels. This file is the second form, both ways:
//
//   * value_from_nbt reads a tag as the component's Codec does (DataComponentType.codecOrThrow
//     over NbtOps) and appends the value in the wire form. What a value may be is not decided
//     here twice: the result goes through components.cpp's own decoder (check_value), which is
//     the Codec's ranges and vanilla's re-encoding.
//   * value_to_nbt writes a stored value as the Codec encodes it - a field left out where the
//     Codec leaves out a default.
//
// Around them ItemStack.CODEC (id, count, components) and DataComponentPatch.CODEC
// ("minecraft:damage": 3, "!minecraft:lore": {}).
//
// Not read, and answered kUnsupported: what components.cpp does not read on the wire either,
// and a registry entry written out in place of its name (an instrument, a painting).
//
// Tick thread: the patch store, and the scratch space of the text codec.
#pragma once

#include <string>
#include <string_view>

#include "item/components.hpp"
#include "item/stack.hpp"
#include "nbt/nbt.hpp"
#include "text/component.hpp"

namespace item {

inline constexpr u32 kNoComponent = ~0u;
std::string_view component_name(u32 type);  // "minecraft:damage"
// Identifier.parse, then the registry: "damage" and "minecraft:damage" are the same. kNoComponent.
u32 find_component(std::string_view name);
// The component has a Codec this server reads: it may stand in a tag.
bool has_codec(u32 type);
// DataComponentType.isTransient: a component that only travels - no Codec, no part of any tag.
bool transient(u32 type);
// A component that has no StreamCodec and so travels as the tag its Codec writes (lock,
// recipes, map_decorations...): `tag` read by that Codec, and appended - its type, its payload -
// as that Codec and vanilla's tag writer put it out again. That is its wire form here too.
Verdict tag_component(u32 type, nbt::Tag tag, base::ByteBuf& out);

// The value of component `type` out of the tag its Codec reads, appended in the wire form.
// kMalformed: the Codec answers with an error.
Verdict value_from_nbt(u32 type, nbt::Tag value, base::ByteBuf& out);
// A stored value as the entry `name` of the compound that is open.
void value_to_nbt(u32 type, std::span<const u8> value, nbt::Writer& w, std::string_view name);

// DataComponentPatch.CODEC: a compound of "type": value and "!type": {}. `out` is the patch
// (0 for an empty one), the caller's reference.
Verdict patch_from_nbt(nbt::Tag components, PatchId& out);
// ItemStack.CODEC: {id, count, components}; an item that does not exist, air or a count outside
// 1..99 is an error. (ItemStack.OPTIONAL_CODEC's empty stack is the caller's to spell.)
Verdict stack_from_nbt(nbt::Tag tag, ItemStack& out);
// ...and its three entries into the compound that is open: id, count, and components if it has any.
void stack_to_nbt(const ItemStack& stack, nbt::Writer& w);
// A patch's entries ("minecraft:damage": 3, "!minecraft:lore": {}) into the compound that is open.
void patch_to_nbt(PatchId patch, nbt::Writer& w);
// The tag of a patch as vanilla writes it into a packet - a compound whose entries, and those
// of every compound in it, are in the order of its HashMap: type and payload.
void patch_to_wire_nbt(PatchId patch, base::ByteBuf& out);

// ---- what an item has of its own (Item.components(): the generated table of every item's
// default components, read through value_from_nbt when first asked for)
struct Default {
    bool has = false;           // the item has this component
    bool read = false;          // ...and its value is one this server reads:
    std::span<const u8> value;  // in the wire form
};
Default default_of(u32 item, u32 type);
// The component as the stack has it: its patch's value, else its item's (kInherit: neither,
// or one that is not read).
Patches::Found effective(const ItemStack& stack, u32 type);
// PatchedDataComponentMap: a patch says only what differs from the item. An entry that sets a
// component to what the item has anyway, or takes away one it does not have, is dropped. Takes
// the caller's reference to `patch` and gives one to what it returns.
PatchId sanitized(u32 item, PatchId patch);
// ItemStack.validateContainedItemSizes: a stack in the stack's container, bundle or crossbow
// that holds more than a stack of its kind may. False if there is one, with its count and that limit.
bool contained_sizes_fit(const ItemStack& stack, u32& count, u32& max);
// How many defaults of components that have a Codec here could not be read: 0, or a bug.
u32 unread_defaults();

// Item.getName(stack) of the items whose class makes the name of what the stack holds: a
// potion's and a tipped arrow's of its contents ("item.minecraft.potion.effect.swiftness"), a
// shield's of its colour, a compass's of its lodestone, a player's head's of whose it is (`arg`:
// the name the translation takes). The translation key; empty for every other item, whose name
// is its ITEM_NAME component.
std::string computed_name_key(const ItemStack& stack, std::string* arg = nullptr);
// ItemStack.getCustomName's second answer: the title of a written book, if it is not blank.
std::string book_title(const ItemStack& stack);

// text::ItemComponents: the components of a stack in a hover event, read and written again.
text::Result hover_components(nbt::Tag components, base::ByteBuf& out);

}  // namespace item
