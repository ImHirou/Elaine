// Version-specific data that is generated offline and only needs framing at startup.
#pragma once

#include "base/buf.hpp"

namespace proto {

// Append the configuration-phase data burst (all registry_data packets, then update_tags) to
// `out`, framed for `threshold`. Built once; sent to every client with a single memcpy.
void build_config_burst(base::ByteBuf& out, i32 threshold);

}  // namespace proto
