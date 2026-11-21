// SPDX-License-Identifier: GPL-3.0-or-later
// Item benchmarks: what a stack costs to read from a client, to hold and to send, and what it
// costs 500 players to look for items lying around them every tick.
#include <random>
#include <string>
#include <vector>

#include "bench.hpp"

#include "entity/grid.hpp"
#include "entity/store.hpp"
#include "item/stack.hpp"
#include "server/inventory.hpp"

#include PROTO_GEN_ITEMS_HPP

namespace benchmark {

namespace {

using item::ItemStack;
namespace et = proto::gen::entity_type;
namespace it = proto::gen::item;
namespace comp = proto::gen::component;

// A stack as set_creative_mode_slot carries it: count, item, patch with length-prefixed values.
std::vector<u8> untrusted(u32 item_id, u32 count, bool enchanted) {
    u8 buf[64];
    u8* p = proto::put_varint(proto::put_varint(buf, count), item_id);
    if (!enchanted) {
        p = proto::put_varint(proto::put_varint(p, 0), 0);
    } else {  // damage = 300, enchantments = {33: 5, 40: 3}
        p = proto::put_varint(proto::put_varint(p, 2), 0);
        p = proto::put_varint(proto::put_varint(proto::put_varint(p, comp::kEnchantments), 5), 2);
        p = proto::put_varint(proto::put_varint(proto::put_varint(proto::put_varint(p, 40), 3), 33), 5);
        p = proto::put_varint(proto::put_varint(proto::put_varint(p, comp::kDamage), 2), 300);
    }
    return {buf, p};
}

// Player.aiStep's look for things to touch, as Game::pick_up_items does it: the cells of the
// item grid under the player's pickup area, then a box test per candidate.
template <class G>
u32 pickup_scan(const entity::Store& s, const G& grid) {
    u32 found = 0;
    for (u32 p = 0; p < s.players(); ++p) {
        const double x0 = s.x[p] - 1.3, x1 = s.x[p] + 1.3, z0 = s.z[p] - 1.3, z1 = s.z[p] + 1.3;
        const double y0 = s.y[p] - 0.5, y1 = s.y[p] + 2.3;
        grid.for_each(G::cell_of(proto::floor_int(x0 - 0.125)), G::cell_of(proto::floor_int(z0 - 0.125)),
                      G::cell_of(proto::floor_int(x1 + 0.125)), G::cell_of(proto::floor_int(z1 + 0.125)), [&](u32 slot) {
                          const u32 e = s.by_slot(slot);
                          found += x0 < s.x[e] + 0.125 && x1 > s.x[e] - 0.125 && z0 < s.z[e] + 0.125 && z1 > s.z[e] - 0.125 &&
                                   y0 < s.y[e] + 0.25 && y1 > s.y[e];
                      });
    }
    return found;
}

}  // namespace

void item_benchmarks() {
    {
        const std::vector<u8> plain = untrusted(it::kStone, 64, false), sword = untrusted(it::kDiamondSword, 1, true);
        ItemStack held;
        {
            proto::Reader r(sword.data(), sword.size());
            item::read_untrusted(r, held);  // keeps the patch alive: the reads below find it interned
        }
        bench("item: read a plain stack from a client", 1, "M/s", [&] {
            proto::Reader r(plain.data(), plain.size());
            ItemStack s;
            item::read_untrusted(r, s);
            g_sink = s.count();
        });
        bench("item: read a stack with 2 components", 1, "M/s", [&] {
            proto::Reader r(sword.data(), sword.size());
            ItemStack s;
            item::read_untrusted(r, s);
            g_sink = s.patch();
        });
        u8 out[64];
        bench("item: write a stack with 2 components", 1, "M/s", [&] { g_sink = *proto::put_item(out, item::view(held)); });
        ItemStack slots[46];
        for (u32 i = 0; i < 46; ++i) slots[i] = i % 5 ? ItemStack(it::kStone, 1 + i) : held;
        bench("item: compare 46 slots (a menu sync)", 46, "M slots/s", [&] {
            u32 same = 0;
            for (u32 i = 0; i < 46; ++i) same += item::matches(slots[i], slots[45 - i]);
            g_sink = same;
        });
    }
    {
        // A menu nothing happened to: what ServerPlayer.tick costs vanilla for every player
        // every tick, and here only for the players something touched.
        server::Menu menu;
        base::ByteBuf out;
        for (u32 i = 0; i < 36; ++i) menu.inv.items[i] = ItemStack(it::kStone, 1 + i);
        menu.send_all(out, 256);
        out.clear();
        bench("menu: sync with nothing to send", 1, "M/s", [&] {
            menu.send_changes(out, 256);
            g_sink = out.len;
        });
    }
    std::mt19937 rng(7);
    const auto uniform = [&](double span) { return (static_cast<double>(rng() % 100000) / 100000.0 - 0.5) * span; };
    // 500 players, 2000 items lying around them, every player looking for what it can reach -
    // through cells of 2 blocks (what the game uses), 4, 8, and 64 (the tracker's: the first version).
    for (const bool crowd : {false, true}) {
        const std::string name =
            std::string("pickup scan, 500 players ") + (crowd ? "in a crowd" : "spread out") + " + 2000 items";
        if (!selected(name.c_str())) continue;
        entity::Store store;
        entity::Store::Moves mv;
        entity::BasicGrid<1> two;
        entity::BasicGrid<2> four;
        entity::BasicGrid<3> eight;
        entity::Grid coarse;
        const double span = crowd ? 40.0 : 5000.0;
        for (u32 i = 0; i < 2000; ++i) {
            const u32 e = store.create(et::kItem, false, Uuid{1, i}, uniform(span), 64, uniform(span), 0, 0, mv);
            const i32 x = proto::floor_int(store.x[e]), z = proto::floor_int(store.z[e]);
            two.add(two.cell_of(x), two.cell_of(z), store.slot[e]);
            four.add(four.cell_of(x), four.cell_of(z), store.slot[e]);
            eight.add(eight.cell_of(x), eight.cell_of(z), store.slot[e]);
            coarse.add(coarse.cell_of(x), coarse.cell_of(z), store.slot[e]);
        }
        for (u32 i = 0; i < 500; ++i) store.create(et::kPlayer, true, Uuid{2, i}, uniform(span), 64, uniform(span), 0, 0, mv);
        bench((name + ", 2-block cells").c_str(), 1e3, "K ticks/s", [&] { g_sink = pickup_scan(store, two); });
        bench((name + ", 4-block cells").c_str(), 1e3, "K ticks/s", [&] { g_sink = pickup_scan(store, four); });
        bench((name + ", 8-block cells").c_str(), 1e3, "K ticks/s", [&] { g_sink = pickup_scan(store, eight); });
        bench((name + ", 64-block cells").c_str(), 1e3, "K ticks/s", [&] { g_sink = pickup_scan(store, coarse); });
    }
}

}  // namespace benchmark
