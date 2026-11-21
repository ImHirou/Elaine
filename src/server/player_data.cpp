// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/player_data.hpp"

#include <cmath>

#include "proto/wire.hpp"

namespace server {

namespace {
constexpr double kMaxHorizontal = 3.0E7, kMaxVertical = 2.0E7;  // where handleMovePlayer clamps a player to
}

void write_player(base::ByteBuf& out, const PlayerData& d) {
    u8* p = out.reserve(2 + 24 + 8 + 1 + proto::kVarIntMaxBytes);
    p = proto::put_u8(p, record::kPlayerVersion);
    p = proto::put_u8(p, static_cast<u8>((d.on_ground ? record::kPlayerOnGround : 0) | (d.flying ? record::kPlayerFlying : 0)));
    p = proto::put_f64(p, d.x);
    p = proto::put_f64(p, d.y);
    p = proto::put_f64(p, d.z);
    p = proto::put_f32(p, d.yaw);
    p = proto::put_f32(p, d.pitch);
    p = proto::put_u8(p, d.selected);
    out.commit(proto::put_varint(p, inv::kSize));
    for (const ItemStack& s : d.items) item::write_untrusted(out, s);
}

bool read_player(std::span<const u8> bytes, PlayerData& out) {
    proto::Reader r(bytes);
    if (r.u8v() != record::kPlayerVersion) return false;
    const u8 flags = r.u8v();
    if (flags & ~(record::kPlayerOnGround | record::kPlayerFlying)) return false;
    out.on_ground = flags & record::kPlayerOnGround;
    out.flying = flags & record::kPlayerFlying;
    out.x = r.f64();
    out.y = r.f64();
    out.z = r.f64();
    out.yaw = r.f32();
    out.pitch = r.f32();
    out.selected = r.u8v();
    const u32 n = r.varint();
    if (!r.ok || n > inv::kSize || out.selected >= inv::kHotbarSize) return false;
    // (The comparisons are false for a NaN.)
    if (!(std::abs(out.x) <= kMaxHorizontal && std::abs(out.z) <= kMaxHorizontal && std::abs(out.y) <= kMaxVertical))
        return false;
    if (!std::isfinite(out.yaw) || !(out.pitch >= -90.0f && out.pitch <= 90.0f)) return false;
    for (u32 i = 0; i < n; ++i) {
        const item::Verdict v = item::read_untrusted(r, out.items[i]);
        if (v == item::Verdict::kMalformed) return false;
        if (v == item::Verdict::kUnsupported) out.items[i] = {};
    }
    for (u32 i = n; i < inv::kSize; ++i) out.items[i] = {};
    return r.done();
}

}  // namespace server
