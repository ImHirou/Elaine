// SPDX-License-Identifier: GPL-3.0-or-later
// Text components: vanilla's Component - the styled text of an item's name and lore, of signs
// and books, of everything a player reads.
//
// A component travels as an NBT tag and is read by a codec (ComponentSerialization.CODEC), not
// by a layout: one text has many spellings - "hi", {text:"hi"}, ["hi"], {type:"text",text:"hi"},
// with keys in any order and unknown ones ignored - and vanilla, having decoded any of them
// into the same object, writes one. transcode() is that round trip without the object: it
// reads a tag as the codec does and appends the tag the codec writes, byte for byte what a
// vanilla server sends for it (compound entries in the order of its HashMap included). So
// equal texts become equal bytes, and nothing a client spelled reaches another client.
//
// Read: plain text, translatable (with fallback and arguments), keybind; children ("extra");
// the whole style - colour (named or #RRGGBB), shadow colour, bold, italic, underlined,
// strikethrough, obfuscated, insertion, font - with click events open_url, run_command,
// suggest_command, change_page, copy_to_clipboard and the hover events show_text, show_entity
// and show_item (its stack's components through the item layer: set_item_components).
// Not read, and answered kUnsupported: score, selector, nbt and object contents; the click
// events show_dialog and custom; a show_item whose stack has a component the item layer does
// not read; a show_entity whose UUID is spelled otherwise than UUID.toString spells it; a shadow colour given as four floats;
// a URL that is not plainly one (Java's URI parser decides those).
#pragma once

#include "base/buf.hpp"
#include "nbt/nbt.hpp"

namespace text {

enum class Result : u8 {
    kOk,
    kUnsupported,  // may well be a component to vanilla, but holds something this server does not read
    kInvalid,      // the codec answers with an error: vanilla throws
};

// Reads the component `tag` describes and, for kOk, appends the tag vanilla writes for it:
// type byte and payload, without a name. Otherwise `out` holds rubbish past its old length.
// Tick thread only (the scratch space is shared).
Result transcode(nbt::Tag tag, base::ByteBuf& out);

// The components of the stack a hover event shows (show_item) are read by the item layer, which
// sits above this one: it hands over what reads `components` as DataComponentPatch.CODEC does
// and appends the payload of the compound vanilla writes for them. Until it has, such a stack
// is kUnsupported.
using ItemComponents = Result (*)(nbt::Tag components, base::ByteBuf& out);
void set_item_components(ItemComponents codec);

}  // namespace text
