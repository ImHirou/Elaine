#include "proto/static_data.hpp"

#include <cstdlib>

#include "proto/frame.hpp"
#include "proto/gen/current.hpp"
#include "proto/wire.hpp"

#include PROTO_GEN_BLOBS_HPP

namespace proto {

void build_config_burst(base::ByteBuf& out, i32 threshold) {
    Reader r(gen::kRegistryDataBlob, sizeof gen::kRegistryDataBlob);
    const u32 count = r.varint();
    for (u32 i = 0; i < count; ++i) {
        const u32 len = r.varint();
        const auto body = r.bytes(len);
        if (!r.ok) std::abort();  // generated data is corrupt: refuse to start
        frame_raw(out, threshold, body);
    }
    frame_raw(out, threshold, {gen::kTagsBlob, sizeof gen::kTagsBlob});
}

}  // namespace proto
