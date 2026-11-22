// SPDX-License-Identifier: GPL-3.0-or-later
// A tag as the text `/data get` shows: NbtUtils.toPrettyComponent.
#pragma once

#include "nbt/nbt.hpp"
#include "text/text.hpp"

namespace text {

// TextComponentTagVisitor("") with its RichStyling: the tag in one line, every token a
// component of its own - names aqua, strings green, numbers gold, type letters red. A list
// shows 128 elements and a tag 64 levels; what is beyond is "<...>". A compound's entries
// come in the order of vanilla's HashMap (nbt::map_order).
Text pretty_nbt(nbt::Tag tag);

}  // namespace text
