// SPDX-License-Identifier: GPL-3.0-or-later
// The records a chunk keeps for the game (server/persist.cpp has the layouts), for the two that
// write them: the game itself, and the import of a vanilla world.
#pragma once

#include <span>

#include "base/buf.hpp"
#include "proto/wire.hpp"

namespace server::record {

// block entities. Version 2 knows one kind more than version 1 (a furnace with the speed of its
// fuel), version 3 one more than that (a sign); otherwise they are the same. A furnace is written
// as the newer kind.
inline constexpr u8 kVersion = 3;
enum : u8 { kKept = 0, kChest = 1, kFurnaceV1 = 2, kFurnace = 3, kSign = 4 };
inline constexpr u8 kSignWaxed = 1, kSignAllowsOp = 2;  // a sign's flags
// entities. Version 2 knows one kind more than version 1 (a falling block); version 3 has a tag
// behind every record: what a tag once said of the entity (its name, its pose...), empty for most.
inline constexpr u8 kEntitiesVersion = 3;
enum : u8 { kItem = 1, kTnt = 2, kOther = 3, kFalling = 4 };
inline constexpr u8 kOnGround = 1, kNoGravity = 2;
inline constexpr u8 kDropItem = 1, kCancelDrop = 2;  // a falling block's flags
inline constexpr u16 kNeverPickUp = 32767;           // ItemEntity.INFINITE_PICKUP_DELAY

inline void put_counted(base::ByteBuf& out, std::span<const u8> bytes) {
    u8* p = out.reserve(proto::kVarIntMaxBytes + bytes.size());
    p = proto::put_varint(p, static_cast<u32>(bytes.size()));
    if (!bytes.empty()) p = proto::put_bytes(p, bytes.data(), bytes.size());  // (an empty span may point nowhere)
    out.commit(p);
}

inline void put_block_entities_head(base::ByteBuf& out, u32 count) {
    u8* p = out.reserve(1 + proto::kVarIntMaxBytes);
    p = proto::put_u8(p, kVersion);
    out.commit(proto::put_varint(p, count));
}

inline void put_block_entity(base::ByteBuf& out, u8 xz, i32 y, u32 type, u8 kind) {
    u8* p = out.reserve(4 + proto::kVarIntMaxBytes);
    p = proto::put_u8(p, xz);
    p = proto::put_be16(p, static_cast<u16>(y));
    p = proto::put_varint(p, type);
    out.commit(proto::put_u8(p, kind));
}

// A stack without components, in the form item::read_untrusted takes; count 0: an empty slot.
inline void put_plain_stack(base::ByteBuf& out, u32 item, u32 count) {
    u8* p = out.reserve(4 * proto::kVarIntMaxBytes);
    p = proto::put_varint(p, count);
    if (count) {
        p = proto::put_varint(p, item);
        p = proto::put_varint(p, 0);  // components added
        p = proto::put_varint(p, 0);  // ...and removed
    }
    out.commit(p);
}

}  // namespace server::record
