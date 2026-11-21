// SPDX-License-Identifier: GPL-3.0-or-later
// What is kept of a player between two visits: where it stood, how it looked about, what it had.
//
// One file a player, `<level-name>/playerstate/<uuid>.bin` (vanilla keeps its own under
// `players/`, which an imported world brings along and this server leaves alone):
//
//     u8      version (1)
//     u8      flags: 1 on the ground, 2 flying
//     f64 x3  position
//     f32 x2  yaw, pitch
//     u8      the selected hotbar slot
//     varint  n, at most inv::kSize
//     n x     a stack of the inventory, by inventory index, in the form item::read_untrusted takes
//
// Big-endian, like everything else that is kept (server/persist.cpp). Ids are this data version's
// (world.json says which). The cursor and the crafting grids are not here: a player who leaves
// drops them (Player.remove), and vanilla saves before that too.
#pragma once

#include <span>

#include "base/buf.hpp"
#include "server/inventory.hpp"

namespace server {

struct PlayerData {
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    bool on_ground = false;
    bool flying = false;
    u8 selected = 0;
    ItemStack items[inv::kSize];
};

namespace record {
inline constexpr u8 kPlayerVersion = 1;
inline constexpr u8 kPlayerOnGround = 1, kPlayerFlying = 2;
}  // namespace record

void write_player(base::ByteBuf& out, const PlayerData& d);
// False: not a file this server can take a player from - another version, a place no player can
// be, bytes that are not a stack. `out` is then not to be used: a player is whole or new. A stack
// with a component this server has no reader for (any more) leaves its slot empty.
bool read_player(std::span<const u8> bytes, PlayerData& out);

}  // namespace server
