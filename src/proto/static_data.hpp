// SPDX-License-Identifier: GPL-3.0-or-later
// Version-specific data that is generated offline and only needs framing at startup.
#pragma once

#include <span>

#include "base/buf.hpp"

namespace proto {

// Append the configuration-phase data burst (all registry_data packets, then update_tags) to
// `out`, framed for `threshold`. Built once; sent to every client with a single memcpy.
void build_config_burst(base::ByteBuf& out, i32 threshold);

// The body of that update_tags packet (packet id, then per registry its tags with the ids of
// their members): the tags as every client was told them, for whoever asks what "#logs" is.
std::span<const u8> tags_body();

}  // namespace proto
