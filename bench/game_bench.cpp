// Benchmarks of what blocks do: recipes, furnaces, an entity against the blocks.
#include <cmath>
#include <vector>

#include "bench.hpp"
#include "item/recipes.hpp"
#include "proto/frame.hpp"
#include "proto/wire.hpp"
#include "server/block_entity.hpp"
#include "world/collide.hpp"
#include "world/generator.hpp"

#include PROTO_GEN_ITEMS_HPP

namespace benchmark {

namespace it = proto::gen::item;
using item::ItemStack;

void game_benchmarks() {
    {
        // A crafting grid is matched on every change of it: a click. The chest is the long
        // case - planks are named by more recipes than anything else - and nine of them in a
        // 3x3 grid match nothing, which walks the whole list.
        ItemStack chest[9], full[9], sticks[9], odd[9];
        for (u32 i = 0; i < 9; ++i) {
            if (i != 4) chest[i] = ItemStack(it::kOakPlanks, 1);
            full[i] = ItemStack(it::kOakPlanks, 1);
        }
        sticks[1] = sticks[4] = ItemStack(it::kOakPlanks, 1);
        odd[0] = ItemStack(it::kIronIngot, 1);
        odd[8] = ItemStack(it::kFlint, 1);
        bench("craft: a chest (8 planks, shaped)", 1, "M/s", [&] { g_sink = item::craft(chest, 3, 3).item(); });
        bench("craft: sticks (2 planks)", 1, "M/s", [&] { g_sink = item::craft(sticks, 3, 3).item(); });
        bench("craft: flint and steel (shapeless)", 1, "M/s", [&] { g_sink = item::craft(odd, 3, 3).item(); });
        bench("craft: 9 planks, no recipe", 1, "M/s", [&] { g_sink = item::craft(full, 3, 3).item(); });
        bench("smelting: the recipe of an item", 1, "M/s", [&] { g_sink = item::smelting(it::kIronOre).result; });
    }
    {
        // Furnaces tick every tick, all of them. Idle ones - nearly all, in a world that has
        // thousands - cost the test at the top of the loop; burning ones the whole serverTick.
        std::vector<server::Furnace> idle(10000), burning(10000);
        for (server::Furnace& f : idle) f.alive = true;
        const auto refill = [&] {
            for (server::Furnace& f : burning) {
                f = {};
                f.alive = true;
                f.set_item(server::Furnace::kInput, ItemStack(it::kCobblestone, 64));
                f.set_item(server::Furnace::kFuel, ItemStack(it::kCoal, 64));
            }
        };
        refill();
        u32 ticks = 0;
        bench("furnace: tick 10 000 idle", 1e3, "K ticks/s", [&] {
            u32 lit = 0;
            for (server::Furnace& f : idle)
                if (!f.idle()) lit += f.tick().lit;
            g_sink = lit;
        });
        bench("furnace: tick 10 000 burning", 1e3, "K ticks/s", [&] {
            if (++ticks % 12000 == 0) refill();  // before the 64 stones are through
            u32 lit = 0;
            for (server::Furnace& f : burning)
                if (!f.idle()) lit += f.tick().lit;
            g_sink = lit;
        });
    }
    {
        // What moving one entity costs: a box the size of a primed TNT against a flat world -
        // falling onto the ground (it is stopped), and the line of sight an explosion asks for.
        world::FlatGenerator gen(-64, 24, world::FlatGenerator::default_layers(), 39);
        world::Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
        dim.ticking(0, 0);
        std::vector<world::Aabb> scratch;
        const world::Aabb resting{{8.01, -60.0, 8.01}, {8.99, -59.02, 8.99}}, flying{{8.01, -50.0, 8.01}, {8.99, -49.02, 8.99}};
        const double fall[3] = {0.01, -0.04, 0.01};
        double out[3];
        bench("collide: a TNT resting on the ground", 1, "M/s", [&] {
            world::collide(dim, resting, fall, out, scratch);
            g_sink = static_cast<u64>(out[1] * 1e9);
        });
        bench("collide: a TNT in the air", 1, "M/s", [&] {
            world::collide(dim, flying, fall, out, scratch);
            g_sink = static_cast<u64>(out[1] * -1e9);
        });
        // The rays of one TNT lying on the ground - and the same asking the world for the block
        // at every point of every ray, which is how it was first written.
        float strengths[world::kBlastRays];
        for (u32 i = 0; i < world::kBlastRays; ++i) strengths[i] = static_cast<float>(i * 2654435761u >> 8) / 16777216.0f;
        world::BlastScratch blast;
        std::vector<world::BlockAt> reached;
        bench("blast: the rays of a TNT on the ground", 1e3, "K/s", [&] {
            world::blast_positions(dim, 8.5, -59.93875, 8.5, 4.0f, strengths, blast, reached);
            g_sink = reached.size();
        });
        bench("blast: the same, a world lookup per point", 1e3, "K/s", [&] {
            u32 ray = 0, hits = 0;
            for (i32 xx = 0; xx < 16; ++xx) {
                for (i32 yy = 0; yy < 16; ++yy) {
                    for (i32 zz = 0; zz < 16; ++zz) {
                        if (xx != 0 && xx != 15 && yy != 0 && yy != 15 && zz != 0 && zz != 15) continue;
                        double xd = static_cast<double>(static_cast<float>(xx) / 15.0f * 2.0f - 1.0f);
                        double yd = static_cast<double>(static_cast<float>(yy) / 15.0f * 2.0f - 1.0f);
                        double zd = static_cast<double>(static_cast<float>(zz) / 15.0f * 2.0f - 1.0f);
                        const double d = std::sqrt(xd * xd + yd * yd + zd * zd);
                        xd /= d, yd /= d, zd /= d;
                        float power = 4.0f * (0.7f + strengths[ray++] * 0.6f);
                        double xp = 8.5, yp = -59.93875, zp = 8.5;
                        for (; power > 0; power -= 0.22500001f) {
                            const world::BlockState s =
                                dim.block(proto::floor_int(xp), proto::floor_int(yp), proto::floor_int(zp));
                            if (!world::is_air(s)) power -= (world::block_info_of(s).explosion_resistance + 0.3f) * 0.3f;
                            hits += power > 0;
                            xp += xd * 0.30000001192092896, yp += yd * 0.30000001192092896, zp += zd * 0.30000001192092896;
                        }
                    }
                }
            }
            g_sink = hits;
        });
        const double from[3] = {8.5, -59.9, 8.5}, to[3] = {14.3, -58.2, 11.7}, under[3] = {12.5, -62.5, 9.5};
        bench("clip: 7 blocks of open air", 1, "M/s", [&] { g_sink = world::clip_hits(dim, from, to); });
        bench("clip: into the ground", 1, "M/s", [&] { g_sink = world::clip_hits(dim, from, under); });
    }
}

}  // namespace benchmark
