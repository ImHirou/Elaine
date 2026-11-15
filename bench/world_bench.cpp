// World benchmarks: the per-block paths (section get/set, chunk lookup), the chunk pipeline
// (generate, light, encode + deflate) and the broadcast scan.
//
// Two worlds are measured. "flat" is what the server generates today. "terrain" is a synthetic
// stand-in for real world generation - rolling surface, water, layered stone with ore sprinkled in
// - so that palettes, light and the compressor see realistic entropy: flat chunks flatter all three.
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "bench.hpp"

#include "server/viewers.hpp"
#include "world/chunk_codec.hpp"
#include "world/dimension.hpp"

namespace benchmark {

using namespace world;
namespace blk = proto::gen::block;

namespace {

constexpr BlockState S(unsigned id) { return static_cast<BlockState>(id); }

// `n` distinct states spread at random: no runs, every palette slot in use.
Section random_section(SectionAllocator& a, u32 n, std::mt19937& rng) {
    std::vector<BlockState> states(Section::kVolume);
    for (auto& s : states) s = S(1 + rng() % n);
    Section s;
    s.assign(a, states.data());
    return s;
}

struct TerrainGenerator final : ChunkGenerator {
    static u32 hash(i32 x, i32 y, i32 z) {
        u32 h = static_cast<u32>(x) * 0x9E3779B1u ^ static_cast<u32>(y) * 0x85EBCA77u ^ static_cast<u32>(z) * 0xC2B2AE3Du;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        return h ^ (h >> 12);
    }
    static i32 height(i32 x, i32 z) {
        return 64 + static_cast<i32>(std::lround(6.0 * std::sin(x * 0.07) + 5.0 * std::cos(z * 0.05) + 3.0 * std::sin((x + z) * 0.11)));
    }
    void generate(Chunk& c, SectionAllocator& a) override {
        static constexpr unsigned kOres[] = {blk::kCoalOre, blk::kIronOre, blk::kCopperOre, blk::kGoldOre,
                                             blk::kRedstoneOre, blk::kLapisOre, blk::kDiamondOre, blk::kEmeraldOre};
        static constexpr unsigned kStones[] = {blk::kStone, blk::kStone, blk::kStone, blk::kGranite, blk::kDiorite, blk::kAndesite, blk::kGravel};
        BlockState states[Section::kVolume];
        for (u32 si = 0; si < c.sections_count; ++si) {
            c.biome[si] = 0;
            const i32 base = -64 + static_cast<i32>(si) * 16;
            if (base > 80) {
                c.sections[si].fill(a, kAir);
                continue;
            }
            for (u32 i = 0; i < Section::kVolume; ++i) {
                const i32 x = c.x * 16 + static_cast<i32>(i & 15), z = c.z * 16 + static_cast<i32>((i >> 4) & 15), y = base + static_cast<i32>(i >> 8);
                const i32 h = height(x, z);
                BlockState s = kAir;
                if (y > h) {
                    s = y <= 62 ? S(blk::kWater) : kAir;
                } else if (y == h) {
                    s = h < 62 ? S(blk::kSand) : S(blk::kGrassBlock);
                } else if (y > h - 4) {
                    s = S(blk::kDirt);
                } else if (y == -64) {
                    s = S(blk::kBedrock);
                } else {
                    const u32 r = hash(x, y, z);
                    if (r % 100 < 3) {
                        s = S(kOres[(r >> 8) % std::size(kOres)]);
                    } else if (y < 0) {
                        s = S((hash(x >> 3, y >> 3, z >> 3) & 7) ? blk::kDeepslate : blk::kTuff);
                    } else {
                        s = S(kStones[hash(x >> 2, y >> 2, z >> 2) % std::size(kStones)]);
                    }
                }
                states[i] = s;
            }
            c.sections[si].assign(a, states);
        }
    }
    i32 sea_level() const override { return 63; }
    i32 spawn_height() const override { return 80; }
    bool is_flat() const override { return false; }
};

// Baseline for the chunk map: a flat open-addressing hash of chunks (what "one hash table" costs
// when it is done well) next to std::unordered_map (what it costs when it is not).
struct FlatChunkHash {
    struct Slot {
        u64 key;
        Chunk* c;
    };
    std::vector<Slot> t;
    u32 mask;
    explicit FlatChunkHash(u32 cap_pow2) : t(cap_pow2, Slot{0, nullptr}), mask(cap_pow2 - 1) {}
    static u64 key(i32 x, i32 z) { return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(z); }
    static u32 hash(u64 k) { return static_cast<u32>((k * 0x9E3779B97F4A7C15ull) >> 32); }
    void insert(i32 x, i32 z, Chunk* c) {
        u32 i = hash(key(x, z)) & mask;
        while (t[i].c) i = (i + 1) & mask;
        t[i] = {key(x, z), c};
    }
    ALWAYS_INLINE Chunk* find(i32 x, i32 z) const {
        const u64 k = key(x, z);
        for (u32 i = hash(k) & mask;; i = (i + 1) & mask) {
            if (!t[i].c) return nullptr;
            if (t[i].key == k) return t[i].c;
        }
    }
};

void section_benchmarks(std::mt19937& rng) {
    SectionAllocator a;
    std::vector<u16> order(Section::kVolume);
    for (u32 i = 0; i < Section::kVolume; ++i) order[i] = static_cast<u16>(i);
    std::shuffle(order.begin(), order.end(), rng);

    if (selected("section memory")) {
        std::printf("section memory, bytes by distinct states:    1: 0   2: %u   4: %u   8: %u   16: %u   32: %u   64: %u   128: %u   256: %u   more: %u\n",
                    Section::block_bytes(1), Section::block_bytes(2), Section::block_bytes(3), Section::block_bytes(4), Section::block_bytes(5),
                    Section::block_bytes(6), Section::block_bytes(7), Section::block_bytes(8), Section::block_bytes(16));
        std::printf("  (vanilla keeps 2048 for 2..16 states, then 2736 / 3280 / 3648 / 4096, then 8192)\n");
    }

    for (u32 n : {1u, 2u, 4u, 16u, 32u, 256u, 1000u}) {
        Section s = random_section(a, n, rng);
        const std::string tag = std::to_string(n) + " states, " + std::to_string(s.bits) + " bits";
        const Section* volatile opaque = &s;  // or the compiler folds a uniform section's loop into a multiply
        bench(("section get, random (" + tag + ")").c_str(), Section::kVolume, "M blocks/s", [&] {
            const Section& sec = *opaque;
            u64 sum = 0;
            for (u16 i : order) sum += sec.get(i);
            g_sink = sum;
        });
        s.release(a);
    }
    {
        // The same 5-bit indices in vanilla's word layout: a division per access when the width is
        // only known at run time, and 12 entries per long instead of 12.8.
        std::vector<u64> words(words_for(Section::kVolume, 5));
        const volatile u32 runtime_bits = 5;
        for (u32 i = 0; i < Section::kVolume; ++i) words_set(words.data(), 5, i, rng() & 31);
        bench("section get, vanilla word layout, 5 bits", Section::kVolume, "M blocks/s", [&] {
            const u32 bits = runtime_bits;
            u64 sum = 0;
            for (u16 i : order) sum += words_get(words.data(), bits, i);
            g_sink = sum;
        });
    }

    for (u32 n : {2u, 16u, 32u, 256u, 1000u}) {
        Section s = random_section(a, n, rng);
        std::vector<BlockState> states(Section::kVolume);
        for (auto& st : states) st = S(1 + rng() % n);
        const std::string tag = std::to_string(n) + " states, " + std::to_string(s.bits) + " bits";
        bench(("section set, random (" + tag + ")").c_str(), Section::kVolume, "M blocks/s", [&] {
            for (u32 k = 0; k < Section::kVolume; ++k) s.set(a, order[k], states[k]);
            g_sink = s.non_air;
        });
        s.release(a);
    }

    for (u32 n : {4u, 32u, 256u}) {
        Section s = random_section(a, n, rng);
        BlockState out[Section::kVolume];
        const std::string tag = std::to_string(n) + " states";
        bench(("section unpack, all 4096 (" + tag + ")").c_str(), Section::kVolume, "M blocks/s", [&] {
            s.unpack(out);
            g_sink = out[77];
        });
        Section t;
        bench(("section assign, all 4096 (" + tag + ")").c_str(), Section::kVolume, "M blocks/s", [&] {
            t.assign(a, out);
            g_sink = t.bits;
        });
        std::vector<u8> wire(section_wire_size(s, 0) + 64);
        bench(("section to wire (" + tag + ")").c_str(), Section::kVolume, "M blocks/s", [&] { g_sink = static_cast<u64>(put_section(wire.data(), s, 0) - wire.data()); });
        t.release(a);
        s.release(a);
    }
}

void chunk_map_benchmarks(std::mt19937& rng) {
    // 100k chunks: 500 players spread out with overlapping views. Fake pointers - none is dereferenced.
    constexpr i32 kSide = 316;
    ChunkMap map;
    FlatChunkHash flat(1u << 18);
    std::unordered_map<u64, Chunk*> stl;
    stl.reserve(static_cast<usize>(kSide) * kSide);
    for (i32 x = 0; x < kSide; ++x)
        for (i32 z = 0; z < kSide; ++z) {
            Chunk* fake = reinterpret_cast<Chunk*>(static_cast<uintptr_t>(0x1000 + (static_cast<u32>(x) * kSide + static_cast<u32>(z)) * 64));
            map.slot(x - kSide / 2, z - kSide / 2) = fake;
            flat.insert(x - kSide / 2, z - kSide / 2, fake);
            stl[FlatChunkHash::key(x - kSide / 2, z - kSide / 2)] = fake;
        }
    constexpr u32 kN = 4096;
    std::vector<i32> xs(kN), zs(kN);
    for (u32 i = 0; i < kN; ++i) {
        xs[i] = static_cast<i32>(rng() % (kSide - 2)) - kSide / 2 + 1;
        zs[i] = static_cast<i32>(rng() % (kSide - 2)) - kSide / 2 + 1;
    }
    const auto stl_find = [&](i32 x, i32 z) {
        const auto it = stl.find(FlatChunkHash::key(x, z));
        return it == stl.end() ? nullptr : it->second;
    };
    // Scattered single lookups: every access a different region.
    bench("chunk map find, scattered (region grid)", kN, "M lookups/s", [&] {
        uintptr_t sum = 0;
        for (u32 i = 0; i < kN; ++i) sum += reinterpret_cast<uintptr_t>(map.find(xs[i], zs[i]));
        g_sink = sum;
    });
    bench("chunk map find, scattered (flat chunk hash)", kN, "M lookups/s", [&] {
        uintptr_t sum = 0;
        for (u32 i = 0; i < kN; ++i) sum += reinterpret_cast<uintptr_t>(flat.find(xs[i], zs[i]));
        g_sink = sum;
    });
    bench("chunk map find, scattered (unordered_map)", kN, "M lookups/s", [&] {
        uintptr_t sum = 0;
        for (u32 i = 0; i < kN; ++i) sum += reinterpret_cast<uintptr_t>(stl_find(xs[i], zs[i]));
        g_sink = sum;
    });
    // What the world actually does: a chunk and its eight neighbours (light, collision, block updates).
    bench("chunk map find, 3x3 (region grid)", kN * 9.0, "M lookups/s", [&] {
        uintptr_t sum = 0;
        for (u32 i = 0; i < kN; ++i)
            for (i32 dx = -1; dx <= 1; ++dx)
                for (i32 dz = -1; dz <= 1; ++dz) sum += reinterpret_cast<uintptr_t>(map.find(xs[i] + dx, zs[i] + dz));
        g_sink = sum;
    });
    bench("chunk map find, 3x3 (flat chunk hash)", kN * 9.0, "M lookups/s", [&] {
        uintptr_t sum = 0;
        for (u32 i = 0; i < kN; ++i)
            for (i32 dx = -1; dx <= 1; ++dx)
                for (i32 dz = -1; dz <= 1; ++dz) sum += reinterpret_cast<uintptr_t>(flat.find(xs[i] + dx, zs[i] + dz));
        g_sink = sum;
    });
    bench("chunk map find, 3x3 (unordered_map)", kN * 9.0, "M lookups/s", [&] {
        uintptr_t sum = 0;
        for (u32 i = 0; i < kN; ++i)
            for (i32 dx = -1; dx <= 1; ++dx)
                for (i32 dz = -1; dz <= 1; ++dz) sum += reinterpret_cast<uintptr_t>(stl_find(xs[i] + dx, zs[i] + dz));
        g_sink = sum;
    });
}

// Passes generate() through and keeps the time spent in it, so that the cost of a (synthetic)
// generator can be told apart from what the dimension does with its output.
struct TimedGenerator final : ChunkGenerator {
    ChunkGenerator& inner;
    double seconds = 0;
    explicit TimedGenerator(ChunkGenerator& g) : inner(g) {}
    void generate(Chunk& c, SectionAllocator& a) override {
        const auto t0 = Clock::now();
        inner.generate(c, a);
        seconds += std::chrono::duration<double>(Clock::now() - t0).count();
    }
    i32 sea_level() const override { return inner.sea_level(); }
    i32 spawn_height() const override { return inner.spawn_height(); }
    bool is_flat() const override { return inner.is_flat(); }
};

// Time the pipeline stages for a view of radius 10 on a fresh dimension.
void pipeline(const char* world, ChunkGenerator& source) {
    const std::string prefix = std::string("pipeline ") + world;
    if (!selected(prefix.c_str())) return;
    constexpr i32 r = 10;
    double load = 1e30, generate = 1e30, light = 1e30, encode = 1e30;
    usize frames = 0, memory = 0, chunks = 0;
    for (int rep = 0; rep < 5; ++rep) {
        TimedGenerator gen(source);
        Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, 256);
        const double all = best_seconds(1, [&] {
            for (i32 x = -r - 2; x <= r + 2; ++x)
                for (i32 z = -r - 2; z <= r + 2; ++z) dim.load(x, z);
        });
        if (all < load) {
            load = all;
            generate = gen.seconds;
        }
        light = std::min(light, best_seconds(1, [&] {
            for (i32 x = -r - 1; x <= r + 1; ++x)
                for (i32 z = -r - 1; z <= r + 1; ++z) dim.lit(x, z);
        }));
        frames = 0;
        encode = std::min(encode, best_seconds(1, [&] {
            for (i32 x = -r; x <= r; ++x)
                for (i32 z = -r; z <= r; ++z) frames += dim.frame(x, z).size();
        }));
        memory = dim.memory_bytes();
        chunks = dim.chunk_count();
    }
    const double n_load = (2 * r + 5) * (2 * r + 5), n_light = (2 * r + 3) * (2 * r + 3), n_enc = (2 * r + 1) * (2 * r + 1);
    std::printf("%-44s %12.2f ms   (%.1f us/chunk)\n", (prefix + ": generator, 625 chunks").c_str(), generate * 1e3, generate * 1e6 / n_load);
    std::printf("%-44s %12.2f ms   (%.1f us/chunk)\n", (prefix + ": heightmaps, sky sources").c_str(), (load - generate) * 1e3,
                (load - generate) * 1e6 / n_load);
    std::printf("%-44s %12.2f ms   (%.1f us/chunk)\n", (prefix + ": light 529 chunks").c_str(), light * 1e3, light * 1e6 / n_light);
    std::printf("%-44s %12.2f ms   (%.1f us/chunk, %.0f bytes/frame)\n", (prefix + ": encode+deflate 441").c_str(), encode * 1e3,
                encode * 1e6 / n_enc, static_cast<double>(frames) / n_enc);
    std::printf("%-44s %12.2f ms   for a first join at view distance 10\n", (prefix + ": total").c_str(), (load + light + encode) * 1e3);
    std::printf("%-44s %12.2f KiB per loaded chunk (%zu chunks, frames included)\n", (prefix + ": memory").c_str(),
                static_cast<double>(memory) / 1024.0 / static_cast<double>(chunks), chunks);
}

void world_runtime(const char* world, ChunkGenerator& gen, i32 ground, std::mt19937& rng) {
    const std::string w = std::string(" (") + world + ")";
    bool any = false;
    for (const char* name : {"world block get, scattered", "cached chunk frame lookup", "place + break stone, with light",
                             "place + break glowstone, with light", "edit + re-encode the chunk frame"})
        any |= selected((name + w).c_str());
    if (!any) return;
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, 256);
    for (i32 x = -3; x <= 3; ++x)
        for (i32 z = -3; z <= 3; ++z) dim.frame(x, z);

    constexpr u32 kN = 4096;
    std::vector<i32> xs(kN), ys(kN), zs(kN);
    for (u32 i = 0; i < kN; ++i) {
        xs[i] = static_cast<i32>(rng() % 112) - 48;
        zs[i] = static_cast<i32>(rng() % 112) - 48;
        ys[i] = ground - 40 + static_cast<i32>(rng() % 48);
    }
    bench(("world block get, scattered" + w).c_str(), kN, "M blocks/s", [&] {
        u64 sum = 0;
        for (u32 i = 0; i < kN; ++i) sum += dim.block(xs[i], ys[i], zs[i]);
        g_sink = sum;
    });
    bench(("cached chunk frame lookup" + w).c_str(), 49, "M frames/s", [&] {
        usize sum = 0;
        for (i32 x = -3; x <= 3; ++x)
            for (i32 z = -3; z <= 3; ++z) sum += dim.frame(x, z).size();
        g_sink = sum;
    });
    // One block edit, end to end: section, heightmaps, sky sources, light, change lists.
    const i32 y = ground + 2;
    bench(("place + break stone, with light" + w).c_str(), 2e3, "K edits/s", [&] {
        dim.set_block(5, y, 5, S(blk::kStone));
        dim.update_light();
        dim.end_tick();
        dim.set_block(5, y, 5, kAir);
        dim.update_light();
        dim.end_tick();
    });
    bench(("place + break glowstone, with light" + w).c_str(), 2e3, "K edits/s", [&] {
        dim.set_block(5, y, 5, S(blk::kGlowstone));
        dim.update_light();
        dim.end_tick();
        dim.set_block(5, y, 5, kAir);
        dim.update_light();
        dim.end_tick();
    });
    // The price of an edit for whoever asks for the chunk next: the frame is rebuilt.
    bench(("edit + re-encode the chunk frame" + w).c_str(), 1e3, "K chunks/s", [&] {
        dim.set_block(5, y, 5, dim.block(5, y, 5) == kAir ? S(blk::kStone) : kAir);
        dim.update_light();
        dim.end_tick();
        g_sink = dim.frame(0, 0).size();
    });
}

void viewer_benchmark(std::mt19937& rng) {
    constexpr u32 kPlayers = 500;
    std::vector<i32> xs(kPlayers), zs(kPlayers), views(kPlayers, 10);
    for (u32 i = 0; i < kPlayers; ++i) {
        xs[i] = static_cast<i32>(rng() % 64) - 32;  // a busy spawn region: many overlapping views
        zs[i] = static_cast<i32>(rng() % 64) - 32;
    }
    std::vector<u32> inside, edge;
    inside.reserve(kPlayers);
    edge.reserve(kPlayers);
    bench("viewer scan, 500 players, one chunk", 1, "M scans/s", [&] {
        inside.clear();
        edge.clear();
        server::find_viewers(xs.data(), zs.data(), views.data(), kPlayers, 3, -7, inside, edge);
        g_sink = inside.size() + edge.size();
    });
}

}  // namespace

void world_benchmarks() {
    std::mt19937 rng(2);
    section_benchmarks(rng);
    chunk_map_benchmarks(rng);
    {
        FlatGenerator flat(-64, 24, FlatGenerator::default_layers(), 0);
        TerrainGenerator terrain;
        pipeline("flat", flat);
        pipeline("terrain", terrain);
        world_runtime("flat", flat, -61, rng);
        world_runtime("terrain", terrain, TerrainGenerator::height(5, 5), rng);
    }
    viewer_benchmark(rng);
}

}  // namespace benchmark
