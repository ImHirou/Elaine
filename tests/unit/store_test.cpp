// SPDX-License-Identifier: GPL-3.0-or-later
// The life of a chunk outside the game: workers, region files, a chunk as bytes, a dimension that
// saves, forgets and reads back - and the import of a vanilla chunk.
//
// The vanilla chunk is written out by hand from the layouts in the jar (nbt_bytes.hpp), never by
// the code under test.
#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <random>

#include "base/workers.hpp"
#include "nbt/nbt.hpp"
#include "nbt_bytes.hpp"
#include "proto/frame.hpp"
#include "proto/wire.hpp"
#include "server/import.hpp"
#include "server/persist.hpp"
#include "world/chunk_codec.hpp"
#include "world/chunk_io.hpp"
#include "world/dimension.hpp"
#include "world/region.hpp"

#include PROTO_GEN_STATIC_IDS_HPP

using namespace world;
namespace blk = proto::gen::block;

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char name[] = "/tmp/store-test-XXXXXX";
        path = ::mkdtemp(name);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

constexpr Layout kOverworld{-64, -4, 24, true};

std::vector<u8> bytes_of(std::mt19937& rng, usize n, bool compressible) {
    std::vector<u8> v(n);
    for (u8& b : v) b = compressible ? static_cast<u8>(rng() % 3) : static_cast<u8>(rng());
    return v;
}

// Hills of stone with glowstone in them and overhangs: light that crosses chunk borders, sections
// that store light only because of the chunk next door.
struct Hills final : ChunkGenerator {
    static u32 hash(i32 x, i32 z) {
        u32 h = static_cast<u32>(x) * 0x9E3779B1u ^ static_cast<u32>(z) * 0x85EBCA77u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        return h ^ (h >> 12);
    }
    static i32 height(i32 x, i32 z) {  // a tower every few columns, low ground between
        const u32 h = hash(x >> 2, z >> 2);
        return (h % 7 == 0) ? 10 + static_cast<i32>(h % 40) : 3 + static_cast<i32>(h % 5);
    }
    void generate(Chunk& c, SectionAllocator& a) override {
        std::vector<BlockState> states(Section::kVolume);
        for (u32 si = 0; si < c.sections_count; ++si) {
            c.biome[si] = 0;
            bool any = false;
            for (u32 i = 0; i < Section::kVolume; ++i) {
                const i32 x = c.x * 16 + static_cast<i32>(i & 15), z = c.z * 16 + static_cast<i32>((i >> 4) & 15);
                const i32 y = static_cast<i32>(si * 16 + (i >> 8));
                const i32 top = height(x, z);
                BlockState s = kAir;
                if (y < top) s = static_cast<BlockState>(hash(x * 31 + y, z) % 97 == 0 ? blk::kGlowstone : blk::kStone);
                if (y == top + 6 && hash(x, z) % 11 == 0) s = static_cast<BlockState>(blk::kGlass);  // something in the air
                states[i] = s;
                any |= s != kAir;
            }
            if (any) c.sections[si].assign(a, states.data());
        }
    }
    i32 sea_level() const override { return 0; }
    i32 spawn_height() const override { return 0; }
    bool is_flat() const override { return false; }
};

std::vector<u8> frame_of(Dimension& dim, i32 x, i32 z) {
    const std::span<const u8> f = dim.frame(x, z);
    return {f.begin(), f.end()};
}

}  // namespace

// ---------------------------------------------------------------------------- workers

TEST(Workers, TasksRunOnWorkersAndComeBackToTheOwner) {
    for (const u32 threads : {0u, 1u, 3u}) {
        base::Workers workers(threads);
        struct State {
            std::atomic<u32> worked{0};
            u32 done = 0;
            u64 sum = 0;
            std::thread::id owner = std::this_thread::get_id();
            bool on_owner = true;
        } state;
        for (u64 i = 1; i <= 500; ++i) {
            base::Task t;
            t.work = [](base::Task& task) {
                static_cast<State*>(task.ctx)->worked.fetch_add(1);
                task.b = task.a * task.a;
            };
            t.done = [](base::Task& task) {
                auto* s = static_cast<State*>(task.ctx);
                ++s->done;
                s->sum += task.b;
                s->on_owner &= std::this_thread::get_id() == s->owner;
            };
            t.ctx = &state;
            t.a = i;
            workers.submit(t, 1 + static_cast<u32>(i % 3));
        }
        EXPECT_EQ(workers.in_flight(), 500u);
        workers.drain();
        EXPECT_EQ(workers.in_flight(), 0u);
        EXPECT_EQ(state.worked.load(), 500u);
        EXPECT_EQ(state.done, 500u);
        EXPECT_EQ(state.sum, 500ull * 501 * 1001 / 6);
        EXPECT_TRUE(state.on_owner) << threads << " threads";
    }
}

// More tasks than a worker's two rings hold, and nobody polls meanwhile: the owner must not end
// up waiting for room to submit while the worker waits for room to answer.
TEST(Workers, MoreTasksAtOnceThanTheRingsHold) {
    base::Workers workers(1);
    u64 sum = 0;
    constexpr u64 kTasks = 30000;
    for (u64 i = 1; i <= kTasks; ++i) {
        base::Task t;
        t.work = [](base::Task& task) { task.b = task.a + 1; };
        t.done = [](base::Task& task) { *static_cast<u64*>(task.ctx) += task.b; };
        t.ctx = &sum;
        t.a = i;
        workers.submit(t);
    }
    workers.drain();
    EXPECT_EQ(sum, kTasks * (kTasks + 1) / 2 + kTasks);
    EXPECT_EQ(workers.in_flight(), 0u);
}

TEST(Workers, ADoneMaySubmitTheNextStep) {
    for (const u32 threads : {0u, 2u}) {
        base::Workers workers(threads);
        struct Chain {
            base::Workers* workers;
            u32 steps = 0;
        } chain{&workers};
        base::Task t;
        t.work = [](base::Task&) {};
        t.done = [](base::Task& task) {
            auto* c = static_cast<Chain*>(task.ctx);
            if (++c->steps < 50) c->workers->submit(task);
        };
        t.ctx = &chain;
        workers.submit(t);
        workers.drain();
        EXPECT_EQ(chain.steps, 50u);
    }
}

// ---------------------------------------------------------------------------- region files

TEST(Region, ChunksComeBackAsTheyWereWritten) {
    TempDir dir;
    const std::string path = dir.path + "/r.0.0.rgn";
    std::mt19937 rng(7);
    std::map<u32, std::vector<u8>> chunks;
    std::string err;
    {
        RegionFile file;
        EXPECT_FALSE(file.open(path, false, err));  // no such file, and none is made
        EXPECT_TRUE(err.empty());
        ASSERT_TRUE(file.open(path, true, err)) << err;
        std::vector<RegionFile::Put> batch;
        for (const u32 index : {0u, 1u, 33u, 512u, 1023u}) {
            chunks[index] = bytes_of(rng, 100 + rng() % 9000, index != 33);
            batch.push_back({index, chunks[index]});
        }
        ASSERT_TRUE(file.write(batch, true, err)) << err;
        std::vector<u8> raw;
        for (const auto& [index, bytes] : chunks) {
            ASSERT_TRUE(region_read(file.fd, file.table[index], raw, err)) << err;
            EXPECT_EQ(raw, bytes);
        }
    }
    RegionFile again;
    ASSERT_TRUE(again.open(path, false, err)) << err;
    std::vector<u8> raw;
    for (u32 i = 0; i < kRegionChunks; ++i) {
        if (!chunks.count(i)) {
            EXPECT_EQ(again.table[i].sector, 0u);
            continue;
        }
        ASSERT_TRUE(region_read(again.fd, again.table[i], raw, err)) << err;
        EXPECT_EQ(raw, chunks[i]);
    }
}

TEST(Region, ARewrittenChunkLeavesItsSectorsToTheNext) {
    TempDir dir;
    const std::string path = dir.path + "/r.-1.2.rgn";
    std::mt19937 rng(3);
    std::string err;
    RegionFile file;
    ASSERT_TRUE(file.open(path, true, err)) << err;
    std::vector<std::vector<u8>> now(kRegionChunks);
    // Many rounds of rewriting a fixed set of chunks: the file must not grow without bound, and
    // no two chunks may ever share a sector.
    u64 largest = 0;
    for (int round = 0; round < 40; ++round) {
        std::vector<RegionFile::Put> batch;
        std::vector<u32> picked;
        for (int k = 0; k < 20; ++k) picked.push_back(rng() % 64);
        std::sort(picked.begin(), picked.end());
        picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        for (const u32 index : picked) {
            now[index] = bytes_of(rng, 200 + rng() % 6000, false);
            batch.push_back({index, now[index]});
        }
        ASSERT_TRUE(file.write(batch, false, err)) << err;
        std::vector<u8> owner(file.sectors, 0);
        for (u32 i = 0; i < kRegionChunks; ++i) {
            const RegionEntry& e = file.table[i];
            if (!e.sector) continue;
            for (u32 s = e.sector; s < e.sector + (e.bytes + kRegionSector - 1) / kRegionSector; ++s) {
                ASSERT_LT(s, file.sectors);
                ASSERT_EQ(owner[s], 0) << "sector " << s << " is in two chunks";
                owner[s] = 1;
            }
        }
        largest = std::max<u64>(largest, file.sectors);
    }
    std::vector<u8> raw;
    usize live = 0;
    for (u32 i = 0; i < 64; ++i) {
        if (now[i].empty()) continue;
        ASSERT_TRUE(region_read(file.fd, file.table[i], raw, err)) << err;
        EXPECT_EQ(raw, now[i]);
        live += now[i].size();
    }
    // Random bytes do not deflate: the file is the chunks, the header and what a rewrite needs
    // beside the copy it replaces.
    EXPECT_LT(largest * kRegionSector, 3 * live + 64 * 1024);
}

TEST(Region, DamageIsFoundNotParsed) {
    TempDir dir;
    const std::string path = dir.path + "/r.0.0.rgn";
    std::mt19937 rng(1);
    std::string err;
    const std::vector<u8> chunk = bytes_of(rng, 5000, true);
    RegionEntry entry;
    {
        RegionFile file;
        ASSERT_TRUE(file.open(path, true, err)) << err;
        const RegionFile::Put put{7, chunk};
        ASSERT_TRUE(file.write({&put, 1}, false, err)) << err;
        entry = file.table[7];
        const u8 flip = 0xFF;
        ASSERT_EQ(::pwrite(file.fd, &flip, 1, static_cast<off_t>(entry.sector) * kRegionSector + 3), 1);
        std::vector<u8> raw;
        EXPECT_FALSE(region_read(file.fd, entry, raw, err));
        EXPECT_EQ(err, "checksum does not match");
    }
    // An entry that points past the end of the file, and one into the header: neither is a chunk.
    {
        RegionFile file;
        ASSERT_TRUE(file.open(path, false, err)) << err;
        RegionEntry bad[2] = {{1u << 20, 100, 100, 0}, {2, 100, 100, 0}};
        ASSERT_EQ(::pwrite(file.fd, bad, sizeof bad, 16 + 16 * 100), static_cast<ssize_t>(sizeof bad));
    }
    RegionFile file;
    ASSERT_TRUE(file.open(path, false, err)) << err;
    EXPECT_EQ(file.table[100].sector, 0u);
    EXPECT_EQ(file.table[101].sector, 0u);
    EXPECT_EQ(file.table[7].sector, entry.sector);
    // Something that is not a region file is not opened as one.
    const std::string other = dir.path + "/r.1.1.rgn";
    {
        RegionFile junk;
        ASSERT_TRUE(junk.open(other, true, err));
        const char text[] = "this is not a region";
        ASSERT_EQ(::pwrite(junk.fd, text, sizeof text, 0), static_cast<ssize_t>(sizeof text));
    }
    RegionFile junk;
    EXPECT_FALSE(junk.open(other, false, err));
    EXPECT_FALSE(err.empty());
}

// ---------------------------------------------------------------------------- a chunk as bytes

TEST(ChunkBytes, SectionsOfEveryWidthBiomesAndLightComeBack) {
    std::mt19937 rng(11);
    ChunkArena arena;
    SectionAllocator alloc(arena);
    Chunk c;
    c.x = -3, c.z = 77;
    c.sections_count = 24, c.light_count = 26;
    std::vector<std::vector<BlockState>> blocks(24, std::vector<BlockState>(Section::kVolume, kAir));
    const u32 distinct[] = {1, 2, 3, 5, 16, 17, 33, 100, 256, 300};
    for (u32 si = 0; si < 10; ++si) {
        std::vector<BlockState> pal;
        while (pal.size() < distinct[si]) pal.push_back(static_cast<BlockState>(rng() % state::kCount));
        for (BlockState& s : blocks[si]) s = pal[rng() % pal.size()];
        c.sections[si].assign(alloc, blocks[si].data());
        c.biome[si] = static_cast<u16>(si);
    }
    // A section that was edited down to one live state and a dead palette slot.
    c.sections[10].set(alloc, 5, static_cast<BlockState>(blk::kStone));
    c.sections[10].set(alloc, 6, static_cast<BlockState>(blk::kGlass));
    c.sections[10].set(alloc, 6, kAir);
    blocks[10][5] = static_cast<BlockState>(blk::kStone);
    BiomeSection biomes[kMaxSections];
    biomes[3].count = 3;
    for (u32 i = 0; i < 64; ++i) biomes[3].index[i] = static_cast<u8>(rng() % 3);
    biomes[3].palette[0] = 7, biomes[3].palette[1] = 1, biomes[3].palette[2] = 40;
    c.biomes = biomes;
    u8 array[LightLayer::kBytes];
    for (u8& b : array) b = static_cast<u8>(rng());
    c.sky[0].v = 1;                                   // stored, never written
    c.sky[1].v = 16;                                  // DataLayer(15)
    c.sky[2].v = reinterpret_cast<uintptr_t>(array);  // an array
    c.block[2].v = 17 + 4;                            // an array that was folded: every nibble 4
    c.status = ChunkStatus::Lit;
    c.modified = true;
    const u8 kept[] = {1, 2, 3, 4, 5};
    c.keep = {sizeof kept, const_cast<u8*>(kept)};
    const u8 be[] = {9, 9, 9}, ents[] = {8, 8};

    base::ByteBuf out;
    encode_chunk(c, {{be, sizeof be}, {ents, sizeof ents}}, out);
    Hills gen;
    LoadedChunk lc;
    std::string err;
    ASSERT_TRUE(load_chunk({out.data, out.len}, -3, 77, kOverworld, gen, lc, err)) << err;
    const Chunk& r = lc.chunk;
    EXPECT_TRUE(r.loaded_lit);
    EXPECT_TRUE(r.modified);
    EXPECT_FALSE(r.dirty);
    EXPECT_FALSE(lc.fresh);
    for (u32 si = 0; si < 24; ++si) {
        for (u32 i = 0; i < Section::kVolume; ++i) ASSERT_EQ(r.sections[si].get(i), blocks[si][i]) << si << " " << i;
        EXPECT_EQ(r.sections[si].non_air, c.sections[si].non_air);
        EXPECT_EQ(r.biome[si], c.biome[si]);
    }
    ASSERT_NE(r.biomes, nullptr);
    EXPECT_EQ(r.biomes[3].count, 3);
    EXPECT_EQ(0, std::memcmp(r.biomes[3].index, biomes[3].index, 64));
    EXPECT_EQ(r.biomes[3].palette[2], 40);
    EXPECT_EQ(r.biomes[0].count, 0);
    EXPECT_EQ(r.sky[0].v, 1u);
    EXPECT_EQ(r.sky[1].v, 16u);
    ASSERT_TRUE(r.sky[2].has_array());
    EXPECT_EQ(0, std::memcmp(r.sky[2].array(), array, LightLayer::kBytes));
    EXPECT_EQ(r.block[2].v, 17u + 4);
    EXPECT_FALSE(r.sky[3].present());
    EXPECT_EQ(std::vector<u8>(lc.parts.block_entities.begin(), lc.parts.block_entities.end()), std::vector<u8>({9, 9, 9}));
    EXPECT_EQ(std::vector<u8>(lc.parts.entities.begin(), lc.parts.entities.end()), std::vector<u8>({8, 8}));
    ASSERT_EQ(r.keep.len, sizeof kept);
    EXPECT_EQ(0, std::memcmp(r.keep.bytes, kept, sizeof kept));
    // What follows from the blocks was worked out again.
    EXPECT_NE(r.filled_sections, 0u);
    EXPECT_NE(r.world_surface.get(0), 0u);

    // The chunk of another place, and one that is cut short anywhere, is refused - never read past.
    LoadedChunk elsewhere;
    EXPECT_FALSE(load_chunk({out.data, out.len}, -3, 78, kOverworld, gen, elsewhere, err));
    for (u32 n = 1; n < out.len; n += std::max<u32>(1, out.len / 257)) {
        LoadedChunk cut;
        EXPECT_FALSE(load_chunk({out.data, n}, -3, 77, kOverworld, gen, cut, err)) << n;
    }
    // Bytes that are not a chunk: every single-byte change is either caught or gives a chunk
    // whose every block is a state that exists.
    std::vector<u8> damaged(out.data, out.data + out.len);
    for (int k = 0; k < 400; ++k) {
        const usize at = rng() % damaged.size();
        const u8 was = damaged[at];
        damaged[at] = static_cast<u8>(rng());
        LoadedChunk d;
        if (load_chunk(damaged, -3, 77, kOverworld, gen, d, err)) {
            for (u32 si = 0; si < 24; ++si)
                for (u32 i = 0; i < Section::kVolume; i += 97) ASSERT_LT(d.chunk.sections[si].get(i), state::kCount);
        }
        damaged[at] = was;
    }
}

TEST(ChunkBytes, NoBytesMeanTheGenerator) {
    Hills gen;
    LoadedChunk lc;
    std::string err;
    ASSERT_TRUE(load_chunk({}, 4, -9, kOverworld, gen, lc, err)) << err;
    EXPECT_TRUE(lc.fresh);
    EXPECT_TRUE(lc.chunk.dirty);
    EXPECT_FALSE(lc.chunk.loaded_lit);
    EXPECT_NE(lc.chunk.filled_sections, 0u);
}

// The biomes of a section on the wire: PalettedContainer over 64 cells (Strategy.createForBiomes) -
// read here as the layout in world/chunk_codec.hpp says, not with anything of the encoder's.
TEST(ChunkBytes, BiomesOfASectionOnTheWire) {
    Section air;  // uniform air: 2 x i16 counts, u8 0, VarInt state
    const auto biomes_of = [&](u32 biome, const BiomeSection* b) {
        std::vector<u8> buf(section_wire_size(air, biome, b) + 64);
        const u8* end = put_section(buf.data(), air, biome, b);
        EXPECT_EQ(static_cast<usize>(end - buf.data()), section_wire_size(air, biome, b));
        return std::vector<u8>(buf.data() + 6, buf.data() + (end - buf.data()));
    };
    // One biome: no bits, its id.
    EXPECT_EQ(biomes_of(41, nullptr), (std::vector<u8>{0, 41}));
    // Two: a palette, one bit a cell, 64 cells in one long.
    BiomeSection two;
    two.count = 2;
    two.palette[0] = 41, two.palette[1] = 15;
    u64 expect = 0;
    for (u32 i = 0; i < 64; ++i) {
        two.index[i] = static_cast<u8>((i * 7 + i / 5) & 1);
        expect |= static_cast<u64>(two.index[i]) << i;
    }
    {
        const std::vector<u8> w = biomes_of(41, &two);
        proto::Reader r(w.data(), w.size());
        EXPECT_EQ(r.u8v(), 1);
        EXPECT_EQ(r.varint(), 2u);
        EXPECT_EQ(r.varint(), 41u);
        EXPECT_EQ(r.varint(), 15u);
        EXPECT_EQ(r.be64(), expect);
        EXPECT_TRUE(r.done());
    }
    // Five: three bits, 21 cells to a long, four longs.
    BiomeSection five;
    five.count = 5;
    for (u32 i = 0; i < 5; ++i) five.palette[i] = static_cast<u16>(10 + i);
    for (u32 i = 0; i < 64; ++i) five.index[i] = static_cast<u8>(i % 5);
    {
        const std::vector<u8> w = biomes_of(10, &five);
        proto::Reader r(w.data(), w.size());
        EXPECT_EQ(r.u8v(), 3);
        ASSERT_EQ(r.varint(), 5u);
        for (u32 i = 0; i < 5; ++i) EXPECT_EQ(r.varint(), 10u + i);
        u64 longs[4];
        for (u64& l : longs) l = r.be64();
        EXPECT_TRUE(r.done());
        for (u32 i = 0; i < 64; ++i) EXPECT_EQ((longs[i / 21] >> (i % 21 * 3)) & 7, i % 5) << i;
    }
    // Nine: more than a palette of three bits holds - the registry's ids, ceillog2(count) bits each.
    BiomeSection nine;
    nine.count = 9;
    for (u32 i = 0; i < 9; ++i) nine.palette[i] = static_cast<u16>(i * 7);
    for (u32 i = 0; i < 64; ++i) nine.index[i] = static_cast<u8>(i % 9);
    {
        const u32 bits = static_cast<u32>(std::bit_width(biome_count() - 1)), per = 64 / bits;
        const std::vector<u8> w = biomes_of(0, &nine);
        proto::Reader r(w.data(), w.size());
        EXPECT_EQ(r.u8v(), bits);
        std::vector<u64> longs((64 + per - 1) / per);
        for (u64& l : longs) l = r.be64();
        EXPECT_TRUE(r.done());
        for (u32 i = 0; i < 64; ++i) EXPECT_EQ((longs[i / per] >> (i % per * bits)) & ((1u << bits) - 1), (i % 9) * 7) << i;
    }
}

// ---------------------------------------------------------------------------- a dimension that keeps its chunks

namespace {

// Builds on a patch of Hills: light sources at chunk borders, blocks taken away, towers put up.
void edit(Dimension& dim) {
    std::mt19937 rng(21);
    for (int k = 0; k < 400; ++k) {
        const i32 x = static_cast<i32>(rng() % 80) - 40, z = static_cast<i32>(rng() % 80) - 40;
        const i32 y = -64 + static_cast<i32>(rng() % 60);
        const u32 what = rng() % 4;
        dim.set_block(x, y, z,
                      what == 0 ? kAir
                                : static_cast<BlockState>(what == 1   ? blk::kGlowstone
                                                          : what == 2 ? blk::kStone
                                                                      : blk::kTorch));
    }
    for (i32 y = -64; y < 20; ++y) dim.set_block(15, y, 3, static_cast<BlockState>(blk::kStone));  // a tower on a border
    dim.set_block(16, 19, 3, static_cast<BlockState>(blk::kGlowstone));
    dim.update_light();
    dim.end_tick();
}

std::map<std::pair<i32, i32>, std::vector<u8>> frames_of(Dimension& dim, i32 r) {
    std::map<std::pair<i32, i32>, std::vector<u8>> out;
    for (i32 x = -r; x <= r; ++x)
        for (i32 z = -r; z <= r; ++z) out[{x, z}] = frame_of(dim, x, z);
    return out;
}

void save_and_unload_all(Dimension& dim) {
    std::vector<Chunk*> all;
    dim.for_each([&](Chunk& c) { all.push_back(&c); });
    for (Chunk* c : all) dim.save(*c, {});
    for (Chunk* c : all) dim.unload(*c);
    dim.stopped().clear();
    EXPECT_EQ(dim.chunk_count(), 0u);
}

}  // namespace

TEST(Persistence, AWorldComesBackAsItWas) {
    TempDir dir;
    Hills gen;
    std::map<std::pair<i32, i32>, std::vector<u8>> before;
    {
        Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression, nullptr,
                      dir.path);
        frames_of(dim, 3);
        edit(dim);
        before = frames_of(dim, 3);
        save_and_unload_all(dim);
    }
    // Another dimension over the same directory: nothing is generated again, nothing lit again,
    // and every chunk a client would be sent is the same bytes - blocks, heightmaps, light.
    struct NoGenerator final : ChunkGenerator {
        void generate(Chunk& c, SectionAllocator&) override {
            ADD_FAILURE() << "chunk " << c.x << ", " << c.z << " was generated";
        }
        i32 sea_level() const override { return 0; }
        i32 spawn_height() const override { return 0; }
        bool is_flat() const override { return false; }
    } none;
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", none, proto::kNoCompression, nullptr, dir.path);
    // (In another order than they were first made in.)
    for (i32 x = 3; x >= -3; --x)
        for (i32 z = -3; z <= 3; ++z) dim.frame(x, z);
    const auto after = frames_of(dim, 3);
    u32 differ = 0;
    for (const auto& [at, frame] : before) differ += after.at(at) != frame;
    EXPECT_EQ(differ, 0u);
    dim.for_each([](Chunk& c) { EXPECT_FALSE(c.dirty) << c.x << ", " << c.z; });
}

TEST(Persistence, AChunkThatLeavesAndReturnsChangesNothingAroundIt) {
    TempDir dir;
    Hills gen;
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression, nullptr, dir.path);
    frames_of(dim, 3);
    edit(dim);
    const auto before = frames_of(dim, 3);
    std::mt19937 rng(5);
    for (int round = 0; round < 12; ++round) {
        // Some chunks leave - their neighbours stay, and lose the light sections they stored
        // only because of them - and are read back: from what waits to be written (the store is
        // not flushed in between), or, every third round, from the file.
        std::vector<Chunk*> leaving;
        for (int k = 0; k < 6; ++k) {
            Chunk* c = dim.find(static_cast<i32>(rng() % 9) - 4, static_cast<i32>(rng() % 9) - 4);
            if (c && std::find(leaving.begin(), leaving.end(), c) == leaving.end()) leaving.push_back(c);
        }
        for (Chunk* c : leaving) dim.save(*c, {});
        for (Chunk* c : leaving) dim.unload(*c);
        for (Chunk* c : dim.stopped()) EXPECT_FALSE(c->ticking);
        dim.stopped().clear();
        dim.update_light();
        dim.end_tick();
        if (round % 3 == 2) dim.store().finish();
        const auto after = frames_of(dim, 3);
        u32 differ = 0;
        for (const auto& [at, frame] : before) differ += after.at(at) != frame;
        ASSERT_EQ(differ, 0u) << "round " << round;
    }
}

TEST(Persistence, RequestedChunksAreLitAndTickAsTheirNeighboursArrive) {
    for (const u32 threads : {0u, 2u}) {
        TempDir dir;
        Hills gen;
        base::Workers workers(threads);
        Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, 256, &workers, dir.path, false);
        for (i32 x = -4; x <= 4; ++x)
            for (i32 z = -4; z <= 4; ++z) dim.request(x, z);
        EXPECT_EQ(dim.loading_count() + dim.chunk_count(), 81u);
        std::vector<Chunk*> ready;
        for (u64 tick = 0; dim.chunk_count() < 81 && tick < 100000; ++tick) {
            dim.pump(tick);
            ready.insert(ready.end(), dim.ready().begin(), dim.ready().end());
            dim.ready().clear();
            if (threads) std::this_thread::yield();
        }
        ASSERT_EQ(dim.chunk_count(), 81u);
        EXPECT_EQ(dim.loading_count(), 0u);
        // A square of radius 4 is loaded: radius 3 is lit, radius 2 ticks - and says so once.
        EXPECT_EQ(ready.size(), 25u);
        for (i32 x = -4; x <= 4; ++x) {
            for (i32 z = -4; z <= 4; ++z) {
                const Chunk* c = dim.find(x, z);
                const i32 d = std::max(std::abs(x), std::abs(z));
                EXPECT_EQ(c->status == ChunkStatus::Lit, d <= 3) << x << ", " << z;
                EXPECT_EQ(c->ticking, d <= 2) << x << ", " << z;
            }
        }
        // A frame is not there until a worker has made it; then it is what the same chunk gives
        // when it is asked for at once.
        Chunk& c = *dim.find(1, -1);
        EXPECT_TRUE(dim.frame_if_ready(c).empty());
        for (u64 tick = 0; dim.frame_if_ready(c).empty() && tick < 100000; ++tick) {
            dim.pump(1000 + tick);
            if (threads) std::this_thread::yield();
        }
        const std::span<const u8> made = dim.frame_if_ready(c);
        ASSERT_FALSE(made.empty());
        const std::vector<u8> by_worker(made.begin(), made.end());
        Dimension at_once(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, 256);
        EXPECT_EQ(frame_of(at_once, 1, -1), by_worker);
        dim.set_block(20, -30, -10, static_cast<BlockState>(blk::kGlass));
        EXPECT_TRUE(dim.frame_if_ready(c).empty());  // it changed: the frame is stale, another is on its way
        dim.update_light();
        dim.end_tick();
    }
}

TEST(Persistence, WithoutADirectoryNothingIsKept) {
    Hills gen;
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    EXPECT_FALSE(dim.persistent());
    dim.frame(0, 0);
    EXPECT_EQ(dim.chunk_count(), 25u);
}

// ---------------------------------------------------------------------------- import

namespace {

using namespace nbt_bytes;

Bytes long_array(std::string_view name, const std::vector<u64>& v) {
    Bytes payload = be32(static_cast<u32>(v.size()));
    for (const u64 x : v) payload = payload + be64(x);
    return entry(12, name, payload);
}
Bytes byte_array(std::string_view name, const Bytes& v) { return entry(7, name, be32(static_cast<u32>(v.size())) + v); }
Bytes lng(std::string_view name, u64 v) { return entry(4, name, be64(v)); }
Bytes named_root(const Bytes& compound_payload) { return Bytes{10} + str("") + compound_payload; }

u32 state_named(std::string_view block, std::initializer_list<std::pair<std::string_view, std::string_view>> props = {}) {
    const u32 id = find_block(block);
    const BlockInfo& info = block_info(id);
    u32 state = info.first_state;
    const u32 offset = static_cast<u32>(info.default_state - info.first_state);
    for (u32 i = 0; i < block_property_count(id); ++i) {
        const BlockProperty p = block_property(id, i);
        u32 index = offset / p.stride % static_cast<u32>(p.values.size());
        for (const auto& [name, value] : props) {
            if (name != p.name) continue;
            for (u32 k = 0; k < p.values.size(); ++k)
                if (p.values[k] == value) index = k;
        }
        state += index * p.stride;
    }
    return state;
}

}  // namespace

TEST(Import, AVanillaChunkBecomesOneOfOurs) {
    // Section Y = -4: bedrock at y = -64, a chest, a furnace and a second chest above it, air
    // elsewhere; 4-bit indices, 16 to a long, index = y << 8 | z << 4 | x.
    std::vector<u64> data(256, 0x2222222222222222ull);  // palette[2]: air
    for (u32 w = 0; w < 16; ++w) data[w] = 0;           // y = -64: palette[0]: bedrock
    const auto put = [&](u32 x, u32 y, u32 z, u64 index) {
        const u32 i = y << 8 | z << 4 | x;
        data[i / 16] = (data[i / 16] & ~(15ull << (i % 16 * 4))) | (index << (i % 16 * 4));
    };
    put(1, 1, 2, 1);  // the chest
    put(5, 1, 6, 3);  // the furnace
    put(9, 1, 9, 1);  // a chest with a name
    put(3, 1, 3, 4);  // a sign
    Bytes sky(2048);
    for (u32 i = 0; i < 2048; ++i) sky[i] = static_cast<u8>(i * 7);
    const Bytes section = compound({
        b("Y", static_cast<u8>(-4)),
        c("block_states",
          {entry(9, "palette",
                 list(10, {compound({s("", "minecraft:bedrock")}),  // a mixed list: plain names travel wrapped
                           compound({s("id", "minecraft:chest"),
                                     c("properties", {s("facing", "east"), s("type", "single"), s("waterlogged", "false")})}),
                           compound({s("", "minecraft:air")}),
                           compound({s("id", "minecraft:furnace"), c("properties", {s("lit", "true")})}),
                           compound({s("id", "minecraft:oak_sign"), c("properties", {s("rotation", "4")})})})),
           long_array("data", data)}),
        c("biomes",
          {l("palette", 8, {str("minecraft:plains"), str("minecraft:desert")}), long_array("data", {0xFFFF0000FFFF0000ull})}),
        byte_array("SkyLight", sky),
    });
    const Bytes upper = compound({b("Y", 5), c("block_states", {l("palette", 8, {str("minecraft:stone")})}),
                                  c("biomes", {l("palette", 8, {str("minecraft:desert")})})});
    const Bytes above_the_world = compound({b("Y", 20), byte_array("BlockLight", Bytes(2048, 0x33))});
    const Bytes chest = compound({s("id", "minecraft:chest"), i("x", 16 * 7 + 1), i("y", -63), i("z", -16 * 3 + 2),
                                  b("keepPacked", 0), entry(10, "components", compound({})),
                                  entry(9, "Items",
                                        list(10, {compound({b("Slot", 0), s("id", "minecraft:stone"), i("count", 5)}),
                                                  compound({b("Slot", 26), s("id", "minecraft:oak_planks")})}))});
    const Bytes furnace =
        compound({s("id", "minecraft:furnace"), i("x", 16 * 7 + 5), i("y", -63), i("z", -16 * 3 + 6),
                  entry(9, "Items", list(10, {compound({b("Slot", 1), s("id", "minecraft:coal"), i("count", 3)})})),
                  entry(2, "lit_time_remaining", be16(100)), entry(2, "lit_total_time", be16(1600)),
                  entry(2, "cooking_time_spent", be16(7)), entry(2, "cooking_total_time", be16(200)),
                  entry(5, "speed_multiplier", be32(0x3F800000)), c("RecipesUsed", {i("minecraft:stone", 3)})});
    const Bytes named = compound({s("id", "minecraft:chest"), i("x", 16 * 7 + 9), i("y", -63), i("z", -16 * 3 + 9),
                                  s("CustomName", "Treasure"), entry(9, "Items", list(0, {}))});
    const Bytes sign =
        compound({s("id", "minecraft:sign"), i("x", 16 * 7 + 3), i("y", -63), i("z", -16 * 3 + 3), b("is_waxed", 1)});
    const Bytes nowhere =
        compound({s("id", "minecraft:chest"), i("x", 16 * 7), i("y", -40), i("z", -16 * 3)});  // on air: vanilla drops it
    const Bytes terrain = named_root(compound({
        i("DataVersion", static_cast<u32>(proto::gen::kWorldVersion)),
        s("Status", "minecraft:full"),
        i("xPos", 7),
        i("zPos", static_cast<u32>(-3)),
        b("isLightOn", 1),
        entry(9, "sections", list(10, {section, upper, above_the_world})),
        entry(9, "block_entities", list(10, {chest, furnace, named, sign, nowhere})),
        lng("InhabitedTime", 12345),
        c("Heightmaps", {}),
    }));
    const Bytes entity_file = named_root(compound({
        i("DataVersion", static_cast<u32>(proto::gen::kWorldVersion)),
        entry(9, "Entities", list(10, {compound({s("id", "minecraft:cow")}), compound({s("id", "minecraft:pig")})})),
    }));

    nbt::Doc terrain_doc, entity_doc;
    ASSERT_EQ(terrain_doc.parse(terrain, nbt::Form::kNamed, nbt::kTrusted), nbt::Error::kNone);
    ASSERT_EQ(entity_doc.parse(entity_file, nbt::Form::kNamed, nbt::kTrusted), nbt::Error::kNone);
    base::ByteBuf out;
    server::ImportStats stats;
    std::string err;
    ASSERT_TRUE(server::import_chunk(terrain_doc, entity_doc, 7, -3, kOverworld, out, stats, err)) << err;
    EXPECT_EQ(stats.chunks, 1u);
    EXPECT_EQ(stats.block_entities, 2u);
    EXPECT_EQ(stats.kept_block_entities, 2u);
    EXPECT_EQ(stats.entities, 2u);

    Hills gen;
    LoadedChunk lc;
    ASSERT_TRUE(load_chunk({out.data, out.len}, 7, -3, kOverworld, gen, lc, err)) << err;
    const Chunk& ch = lc.chunk;
    EXPECT_TRUE(ch.loaded_lit);
    EXPECT_EQ(ch.sections[0].get(0, 0, 0), blk::kBedrock);
    EXPECT_EQ(ch.sections[0].get(1, 1, 2), state_named("minecraft:chest", {{"facing", "east"}}));
    EXPECT_EQ(ch.sections[0].get(5, 1, 6), state_named("minecraft:furnace", {{"lit", "true"}}));
    EXPECT_EQ(ch.sections[0].get(3, 1, 3), state_named("minecraft:oak_sign", {{"rotation", "4"}}));
    EXPECT_EQ(ch.sections[0].get(8, 8, 8), kAir);
    EXPECT_EQ(ch.sections[9].get(4, 4, 4), blk::kStone);
    EXPECT_TRUE(ch.sections[9].uniform());
    // Biomes: two in the lowest section, cell by cell; one in the other.
    ASSERT_NE(ch.biomes, nullptr);
    EXPECT_EQ(ch.biomes[0].count, 2);
    EXPECT_EQ(ch.biomes[0].palette[0], proto::reg::worldgen_biome::kPlains);
    EXPECT_EQ(ch.biomes[0].palette[1], proto::reg::worldgen_biome::kDesert);
    for (u32 cell = 0; cell < 64; ++cell) EXPECT_EQ(ch.biomes[0].index[cell], (cell / 16) & 1) << cell;
    EXPECT_EQ(ch.biomes[9].count, 0);
    EXPECT_EQ(ch.biome[9], proto::reg::worldgen_biome::kDesert);
    // Light: the layers that were saved, by light section (one below the world is 0).
    ASSERT_TRUE(ch.sky[1].has_array());
    EXPECT_EQ(0, std::memcmp(ch.sky[1].array(), sky.data(), 2048));
    EXPECT_FALSE(ch.block[1].present());
    ASSERT_TRUE(ch.block[25].has_array());
    EXPECT_EQ(ch.block[25].array()[100], 0x33);

    // Block entities: the layout of server/persist.cpp, read by hand.
    proto::Reader r(lc.parts.block_entities.data(), lc.parts.block_entities.size());
    EXPECT_EQ(r.u8v(), 2);
    ASSERT_EQ(r.varint(), 4u);
    const auto stack = [&]() -> std::pair<u32, u32> {
        const u32 count = r.varint();
        if (!count) return {0, 0};
        const u32 item = r.varint();
        EXPECT_EQ(r.varint(), 0u);
        EXPECT_EQ(r.varint(), 0u);
        return {item, count};
    };
    // ...the chest
    EXPECT_EQ(r.u8v(), 1 << 4 | 2);
    EXPECT_EQ(static_cast<i16>(r.be16()), -63);
    EXPECT_EQ(r.varint(), proto::gen::block_entity_type::kChest);
    EXPECT_EQ(r.u8v(), server::record::kChest);
    for (u32 slot = 0; slot < 27; ++slot) {
        const auto [item, count] = stack();
        if (slot == 0) {
            EXPECT_EQ(item, find_item("minecraft:stone"));
            EXPECT_EQ(count, 5u);
        } else if (slot == 26) {
            EXPECT_EQ(item, find_item("minecraft:oak_planks"));
            EXPECT_EQ(count, 1u);  // "count" left out is one
        } else {
            EXPECT_EQ(count, 0u);
        }
    }
    EXPECT_EQ(r.varint(), 0u);
    // ...the furnace, with what it has cooked kept beside it
    EXPECT_EQ(r.u8v(), 5 << 4 | 6);
    EXPECT_EQ(static_cast<i16>(r.be16()), -63);
    EXPECT_EQ(r.varint(), proto::gen::block_entity_type::kFurnace);
    EXPECT_EQ(r.u8v(), server::record::kFurnace);
    EXPECT_EQ(stack().second, 0u);
    EXPECT_EQ(stack(), (std::pair<u32, u32>{find_item("minecraft:coal"), 3}));
    EXPECT_EQ(stack().second, 0u);
    EXPECT_EQ(r.be32(), 100u);
    EXPECT_EQ(r.be32(), 1600u);
    EXPECT_EQ(r.be32(), 7u);
    EXPECT_EQ(r.be32(), 200u);
    EXPECT_EQ(r.f32(), 1.0f);  // speed_multiplier
    {
        const std::span<const u8> kept = r.bytes(r.varint());
        nbt::Doc doc;
        ASSERT_EQ(doc.parse(kept), nbt::Error::kNone);
        EXPECT_EQ(doc.root().get("RecipesUsed").get("minecraft:stone").as_int(), 3);
    }
    // ...the chest with a name: the tag it came as, whole
    EXPECT_EQ(r.u8v(), 9 << 4 | 9);
    r.be16();
    EXPECT_EQ(r.varint(), proto::gen::block_entity_type::kChest);
    EXPECT_EQ(r.u8v(), server::record::kKept);
    {
        const std::span<const u8> kept = r.bytes(r.varint());
        nbt::Doc doc;
        ASSERT_EQ(doc.parse(kept), nbt::Error::kNone);
        EXPECT_EQ(doc.root().get("CustomName").str(), "Treasure");
        EXPECT_EQ(doc.root().get("x").as_int(), 16 * 7 + 9);
    }
    // ...and the sign
    EXPECT_EQ(r.u8v(), 3 << 4 | 3);
    r.be16();
    EXPECT_EQ(r.varint(), proto::gen::block_entity_type::kSign);
    EXPECT_EQ(r.u8v(), server::record::kKept);
    r.bytes(r.varint());
    EXPECT_TRUE(r.done());

    // What nobody reads: the chunk's other entries, and its entities, as tags.
    ASSERT_GT(ch.keep.len, 9u);
    EXPECT_EQ(ch.keep.bytes[0], 1);
    const u32 other = load_le<u32>(ch.keep.bytes + 1);
    nbt::Doc rest;
    ASSERT_EQ(rest.parse({ch.keep.bytes + 5, other}), nbt::Error::kNone);
    EXPECT_EQ(rest.root().get("InhabitedTime").as_long(), 12345);
    EXPECT_FALSE(rest.root().get("sections"));
    EXPECT_FALSE(rest.root().get("Heightmaps"));
    const u8* at = ch.keep.bytes + 5 + other;
    ASSERT_EQ(load_le<u32>(at), 2u);
    nbt::Doc cow;
    ASSERT_EQ(cow.parse({at + 8, load_le<u32>(at + 4)}), nbt::Error::kNone);
    EXPECT_EQ(cow.root().get("id").str(), "minecraft:cow");
}

TEST(Import, WhatIsNotAChunkOfThisVersionIsRefusedOrLeftToTheGenerator) {
    server::ImportStats stats;
    std::string err;
    nbt::Doc none;
    // Another data version: refused, with words that say what to do.
    {
        const Bytes old = named_root(compound({i("DataVersion", 3955), s("Status", "minecraft:full")}));
        nbt::Doc doc;
        ASSERT_EQ(doc.parse(old, nbt::Form::kNamed, nbt::kTrusted), nbt::Error::kNone);
        base::ByteBuf out;
        EXPECT_FALSE(server::import_chunk(doc, none, 0, 0, kOverworld, out, stats, err));
        EXPECT_NE(err.find("3955"), std::string::npos) << err;
    }
    // A block that does not exist.
    {
        const Bytes odd = named_root(
            compound({i("DataVersion", static_cast<u32>(proto::gen::kWorldVersion)), s("Status", "minecraft:full"),
                      entry(9, "sections",
                            list(10, {compound({b("Y", 0), c("block_states", {l("palette", 8, {str("mod:machine")})})})}))}));
        nbt::Doc doc;
        ASSERT_EQ(doc.parse(odd, nbt::Form::kNamed, nbt::kTrusted), nbt::Error::kNone);
        base::ByteBuf out;
        EXPECT_FALSE(server::import_chunk(doc, none, 0, 0, kOverworld, out, stats, err));
        EXPECT_NE(err.find("mod:machine"), std::string::npos) << err;
    }
    // A chunk vanilla had not finished: nothing is kept of it, the generator will make it...
    const Bytes proto_chunk = named_root(compound({i("DataVersion", static_cast<u32>(proto::gen::kWorldVersion)),
                                                   s("Status", "minecraft:features"), lng("InhabitedTime", 5)}));
    nbt::Doc unfinished;
    ASSERT_EQ(unfinished.parse(proto_chunk, nbt::Form::kNamed, nbt::kTrusted), nbt::Error::kNone);
    base::ByteBuf out;
    stats = {};
    ASSERT_TRUE(server::import_chunk(unfinished, none, 2, 2, kOverworld, out, stats, err)) << err;
    EXPECT_EQ(out.len, 0u);
    EXPECT_EQ(stats.unfinished, 1u);
    // ...unless entities were saved for it: those are kept, on blocks the generator makes.
    const Bytes entity_file = named_root(compound({i("DataVersion", static_cast<u32>(proto::gen::kWorldVersion)),
                                                   entry(9, "Entities", list(10, {compound({s("id", "minecraft:bat")})}))}));
    nbt::Doc entities;
    ASSERT_EQ(entities.parse(entity_file, nbt::Form::kNamed, nbt::kTrusted), nbt::Error::kNone);
    ASSERT_TRUE(server::import_chunk(unfinished, entities, 2, 2, kOverworld, out, stats, err)) << err;
    ASSERT_GT(out.len, 0u);
    Hills gen;
    LoadedChunk lc;
    ASSERT_TRUE(load_chunk({out.data, out.len}, 2, 2, kOverworld, gen, lc, err)) << err;
    EXPECT_NE(lc.chunk.filled_sections, 0u);  // Hills made it
    EXPECT_FALSE(lc.fresh);
    EXPECT_TRUE(lc.chunk.dirty);
    EXPECT_GT(lc.chunk.keep.len, 9u);
}
