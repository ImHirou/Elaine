// Section storage, the chunk map and the chunk wire format.
//
// The wire tests decode with an independent reader written from the vanilla layout (see the comment
// in world/chunk_codec.hpp for where it was read), not with anything from the encoder.
#include <gtest/gtest.h>

#include <map>
#include <random>
#include <set>
#include <vector>

#include "proto/frame.hpp"
#include "proto/wire.hpp"
#include "world/chunk_codec.hpp"
#include "world/chunk_map.hpp"
#include "world/collide.hpp"
#include "world/dimension.hpp"
#include "world/section.hpp"

using namespace world;
namespace blk = proto::gen::block;

namespace {

// A spread of real states: air kinds, opaque, transparent, fluid, light sources.
std::vector<BlockState> some_states(usize n, std::mt19937& rng) {
    std::set<BlockState> s = {kAir, static_cast<BlockState>(blk::kStone), static_cast<BlockState>(blk::kWater),
                              static_cast<BlockState>(blk::kGlass), static_cast<BlockState>(blk::kGlowstone)};
    while (s.size() < n) s.insert(static_cast<BlockState>(rng() % state::kCount));
    std::vector<BlockState> v(s.begin(), s.end());
    std::shuffle(v.begin(), v.end(), rng);
    v.resize(n);
    return v;
}

void expect_matches(const Section& s, const std::vector<BlockState>& ref) {
    u32 solid = 0, wet = 0;
    for (u32 i = 0; i < Section::kVolume; ++i) {
        ASSERT_EQ(s.get(i), ref[i]) << "index " << i << " bits " << int(s.bits);
        solid += !is_air(ref[i]);
        wet += (flags_of(ref[i]) & state::kHasFluid) != 0;
    }
    EXPECT_EQ(s.non_air, solid);
    EXPECT_EQ(s.fluids, wet);
    BlockState out[Section::kVolume];
    s.unpack(out);
    EXPECT_EQ(0, std::memcmp(out, ref.data(), sizeof out));
}

u32 distinct(const std::vector<BlockState>& v) { return static_cast<u32>(std::set<BlockState>(v.begin(), v.end()).size()); }

// ---- independent wire readers (vanilla layout)

struct Container {
    u32 bits = 0;
    std::vector<u32> palette;
    std::vector<u32> values;
};

// PalettedContainer: u8 bits; palette; longs without a count. `direct_from` = first width without a palette.
Container read_container(proto::Reader& r, u32 entries, u32 direct_from) {
    Container c;
    c.bits = r.u8v();
    if (c.bits == 0) {
        c.palette.push_back(r.varint());
        c.values.assign(entries, c.palette[0]);
        return c;
    }
    if (c.bits < direct_from) {
        const u32 n = r.varint();
        for (u32 i = 0; i < n; ++i) c.palette.push_back(r.varint());
    }
    const u32 per_long = 64 / c.bits;
    const u32 longs = (entries + per_long - 1) / per_long;
    for (u32 j = 0; j < longs; ++j) {
        const u64 w = r.be64();
        for (u32 k = 0; k < per_long && c.values.size() < entries; ++k) {
            const u32 v = static_cast<u32>(w >> (k * c.bits)) & ((1u << c.bits) - 1);
            c.values.push_back(c.bits < direct_from ? (v < c.palette.size() ? c.palette[v] : ~0u) : v);
        }
    }
    return c;
}

std::vector<u64> read_long_array(proto::Reader& r) {
    std::vector<u64> v(r.varint());
    for (u64& x : v) x = r.be64();
    return v;
}

// ByteBufCodecs.BIT_SET: VarInt byte count, then BitSet.toByteArray() - little-endian, no trailing zero byte.
u64 read_bit_set(proto::Reader& r) {
    const auto bytes = r.byte_array(8);
    EXPECT_TRUE(bytes.empty() || bytes.back() != 0) << "bit set with a trailing zero byte";
    u64 v = 0;
    for (usize i = 0; i < bytes.size(); ++i) v |= u64{bytes[i]} << (8 * i);
    return v;
}

struct VoidGenerator final : ChunkGenerator {
    void generate(Chunk& c, SectionAllocator&) override {
        for (u32 si = 0; si < c.sections_count; ++si) c.biome[si] = 7;
    }
    i32 sea_level() const override { return 63; }
    i32 spawn_height() const override { return 64; }
    bool is_flat() const override { return false; }
};

}  // namespace

// ---------------------------------------------------------------------------- bits

TEST(Bits, DenseRoundTripEveryWidth) {
    std::mt19937 rng(7);
    for (u32 bits = 1; bits <= 8; ++bits) {
        std::vector<u8> idx(kSectionVolume), back(kSectionVolume);
        for (u8& b : idx) b = static_cast<u8>(rng() & ((1u << bits) - 1));
        std::vector<u8> dense(packed_bytes(bits) + kPackedSlack, 0xAA);
        pack_indices(idx.data(), bits, dense.data());
        for (u32 i = 0; i < kSectionVolume; ++i) ASSERT_EQ(packed_get(dense.data(), bits, i), idx[i]) << bits << " @" << i;
        unpack_indices(dense.data(), bits, back.data());
        EXPECT_EQ(idx, back);
        // Random single-entry writes against the byte array.
        for (int n = 0; n < 5000; ++n) {
            const u32 i = rng() % kSectionVolume, v = rng() & ((1u << bits) - 1);
            ASSERT_EQ(packed_exchange(dense.data(), bits, i, v), idx[i]);
            idx[i] = static_cast<u8>(v);
        }
        unpack_indices(dense.data(), bits, back.data());
        EXPECT_EQ(idx, back);
    }
}

TEST(Bits, WordLayoutMatchesSimpleBitStorage) {
    // 256 entries of 9 bits: 7 per long, 37 longs, the last one holding 4 (Heightmap).
    EXPECT_EQ(words_for(256, 9), 37u);
    EXPECT_EQ(words_for(4096, 5), 342u);
    EXPECT_EQ(words_for(4096, 4), 256u);
    EXPECT_EQ(words_for(4096, 16), 1024u);
    std::mt19937 rng(3);
    ColumnMap m;
    m.fill(0);
    u32 ref[256];
    for (u32 i = 0; i < 256; ++i) {
        ref[i] = rng() % 385;
        m.set(i, ref[i]);
    }
    for (u32 i = 0; i < 256; ++i) {
        ASSERT_EQ(m.get(i), ref[i]);
        ASSERT_EQ((m.w[i / 7] >> ((i % 7) * 9)) & 511, ref[i]);  // the definition
    }
    m.fill(300);
    for (u32 i = 0; i < 256; ++i) ASSERT_EQ(m.get(i), 300u);
    EXPECT_EQ(m.w[36] >> 36, 0u);  // unused bits of the last long stay zero
}

// ---------------------------------------------------------------------------- section

TEST(Section, StartsUniformAndAllocatesOnSecondState) {
    SectionAllocator a;
    Section s;
    EXPECT_EQ(sizeof(Section), 32u);
    EXPECT_TRUE(s.uniform());
    EXPECT_TRUE(s.only_air());
    EXPECT_EQ(s.set(a, 5, kAir), kAir);
    EXPECT_EQ(a.live_blocks(), 0u);
    EXPECT_EQ(s.set(a, 5, blk::kStone), kAir);
    EXPECT_EQ(s.bits, 1);
    EXPECT_EQ(a.live_blocks(), 1u);
    EXPECT_EQ(s.get(5), blk::kStone);
    EXPECT_EQ(s.get(4), kAir);
    EXPECT_EQ(s.non_air, 1);
    EXPECT_EQ(s.set(a, 5, blk::kStone), blk::kStone);  // unchanged
    EXPECT_EQ(s.non_air, 1);
    // Back to one state everywhere: the block goes back to the pool.
    EXPECT_EQ(s.set(a, 5, kAir), blk::kStone);
    EXPECT_TRUE(s.uniform());
    EXPECT_EQ(a.live_blocks(), 0u);
    EXPECT_EQ(s.non_air, 0);
}

TEST(Section, WidthIsExactForThePaletteSize) {
    SectionAllocator a;
    Section s;
    std::mt19937 rng(11);
    const auto states = some_states(300, rng);
    std::vector<BlockState> ref(Section::kVolume, kAir);
    // states[0] may be air itself; start from a known base and add one new state at a time.
    u32 seen = 1;
    std::set<BlockState> have = {kAir};
    for (u32 n = 0; n < 300; ++n) {
        const BlockState st = states[n];
        for (u32 k = 0; k < 3; ++k) {  // a few blocks of each
            const u32 i = rng() % Section::kVolume;
            s.set(a, i, st);
            ref[i] = st;
        }
        have.insert(st);
        seen = static_cast<u32>(have.size());
        // Slots are only reused when a state disappears entirely, so the palette never needs more
        // bits than the number of states ever seen.
        if (seen <= 256) {
            EXPECT_LE(s.bits, Section::bits_for(seen)) << "after " << seen << " states";
            EXPECT_GE(s.bits, Section::bits_for(distinct(ref)));
        }
    }
    EXPECT_EQ(s.bits, Section::kDirectBits);
    expect_matches(s, ref);
    s.release(a);
    EXPECT_EQ(a.live_blocks(), 0u);
}

TEST(Section, RandomEditsMatchAReferenceArray) {
    std::mt19937 rng(42);
    for (usize palette : {2u, 3u, 4u, 5u, 9u, 16u, 17u, 33u, 100u, 256u, 257u, 700u}) {
        SectionAllocator a;
        Section s;
        const auto states = some_states(palette, rng);
        std::vector<BlockState> ref(Section::kVolume, kAir);
        for (int n = 0; n < 30000; ++n) {
            const u32 i = rng() % Section::kVolume;
            const BlockState st = states[rng() % states.size()];
            ASSERT_EQ(s.set(a, i, st), ref[i]);
            ref[i] = st;
        }
        expect_matches(s, ref);
        // Wipe most of it: the palette keeps dead slots, compact() shrinks to what is left.
        for (u32 i = 0; i < Section::kVolume; ++i) {
            if (i % 97 == 0) continue;
            s.set(a, i, states[0]);
            ref[i] = states[0];
        }
        expect_matches(s, ref);
        s.compact(a);
        expect_matches(s, ref);
        const u32 d = distinct(ref);
        EXPECT_EQ(s.bits, d == 1 ? 0u : Section::bits_for(d)) << "palette " << palette << ", " << d << " left";
        if (s.bits >= 1 && s.bits <= 8) {
            // Dead slots that do not cost a bit are kept for reuse; the live ones are exactly the states left.
            EXPECT_EQ(static_cast<u32>(s.palette_len - s.dead), d);
            EXPECT_LE(s.palette_len, 1u << s.bits);
        }
        s.release(a);
        EXPECT_EQ(a.live_blocks(), 0u);
    }
}

TEST(Section, DeadSlotsAreReusedBeforeGrowing) {
    SectionAllocator a;
    Section s;
    s.set(a, 0, blk::kStone);
    s.set(a, 1, blk::kDirt);
    s.set(a, 2, blk::kGlass);  // air, stone, dirt, glass = 4 states = 2 bits, palette full
    EXPECT_EQ(s.bits, 2);
    EXPECT_EQ(s.palette_len, 4);
    s.set(a, 1, kAir);  // dirt is gone
    EXPECT_EQ(s.dead, 1);
    s.set(a, 3, blk::kBedrock);  // takes dirt's slot instead of growing to 3 bits
    EXPECT_EQ(s.bits, 2);
    EXPECT_EQ(s.palette_len, 4);
    EXPECT_EQ(s.dead, 0);
    EXPECT_EQ(s.get(3), blk::kBedrock);
    EXPECT_EQ(s.get(1), kAir);
    s.set(a, 4, blk::kDirt);  // a fifth state: now it has to grow
    EXPECT_EQ(s.bits, 3);
    EXPECT_EQ(s.get(4), blk::kDirt);
    s.release(a);
}

TEST(Section, AssignCopyAndFill) {
    std::mt19937 rng(5);
    for (usize palette : {1u, 2u, 7u, 40u, 256u, 400u}) {
        SectionAllocator a;
        const auto states = some_states(palette, rng);
        std::vector<BlockState> ref(Section::kVolume);
        for (auto& r : ref) r = states[rng() % states.size()];
        Section s;
        s.assign(a, ref.data());
        expect_matches(s, ref);
        const u32 d = distinct(ref);
        EXPECT_EQ(s.bits, d == 1 ? 0u : Section::bits_for(d));
        Section t;
        t.set(a, 9, blk::kStone);  // has a block of its own that copy_from must release
        t.copy_from(a, s);
        expect_matches(t, ref);
        t.set(a, 0, ref[0] == blk::kStone ? blk::kDirt : blk::kStone);  // the copy is independent
        EXPECT_EQ(s.get(0), ref[0]);
        t.fill(a, blk::kWater);
        EXPECT_TRUE(t.uniform());
        EXPECT_EQ(t.non_air, 4096);
        EXPECT_EQ(t.fluids, 4096);
        s.release(a);
        t.release(a);
        EXPECT_EQ(a.live_blocks(), 0u);
    }
}

TEST(Section, DirectStorageFallsBackToAPaletteWhenItFitsAgain) {
    SectionAllocator a;
    Section s;
    std::mt19937 rng(9);
    const auto states = some_states(600, rng);
    std::vector<BlockState> ref(Section::kVolume, kAir);
    for (u32 i = 0; i < Section::kVolume; ++i) s.set(a, i, ref[i] = states[i % 600]);
    EXPECT_EQ(s.bits, Section::kDirectBits);
    s.compact(a);
    EXPECT_EQ(s.bits, Section::kDirectBits);  // still 600 states
    for (u32 i = 0; i < Section::kVolume; ++i) s.set(a, i, ref[i] = states[i % 50]);
    expect_matches(s, ref);
    s.compact(a);
    EXPECT_EQ(s.bits, Section::bits_for(50));
    expect_matches(s, ref);
    s.release(a);
}

// ---------------------------------------------------------------------------- chunk map

TEST(ChunkMap, FindsWhatWasInsertedAcrossRegionBorders) {
    ChunkMap map;
    std::map<std::pair<i32, i32>, Chunk*> ref;
    std::vector<std::unique_ptr<Chunk>> own;
    std::mt19937 rng(1);
    const auto add = [&](i32 x, i32 z) {
        if (ref.count({x, z})) return;
        own.push_back(std::make_unique<Chunk>());
        own.back()->x = x;
        own.back()->z = z;
        map.insert(own.back().get());
        ref[{x, z}] = own.back().get();
    };
    for (i32 x = -40; x <= 40; ++x)
        for (i32 z = -3; z <= 3; ++z) add(x, z);  // crosses region borders at -33/-32, -1/0, 31/32
    for (int i = 0; i < 2000; ++i) add(static_cast<i32>(rng() % 4000) - 2000, static_cast<i32>(rng() % 4000) - 2000);
    add(1875000, -1875000);  // the edge of the world
    add(-1875000, 1874999);
    EXPECT_EQ(map.size(), ref.size());
    for (const auto& [k, c] : ref) ASSERT_EQ(map.find(k.first, k.second), c) << k.first << "," << k.second;
    for (int i = 0; i < 20000; ++i) {
        const i32 x = static_cast<i32>(rng() % 4200) - 2100, z = static_cast<i32>(rng() % 4200) - 2100;
        const auto it = ref.find({x, z});
        ASSERT_EQ(map.find(x, z), it == ref.end() ? nullptr : it->second);
    }
    usize seen = 0;
    map.for_each([&](Chunk& c) {
        ++seen;
        EXPECT_EQ(ref.at({c.x, c.z}), &c);
    });
    EXPECT_EQ(seen, ref.size());
    EXPECT_EQ(map.remove(0, 0), ref.at({0, 0}));
    EXPECT_EQ(map.find(0, 0), nullptr);
    EXPECT_EQ(map.remove(0, 0), nullptr);
    EXPECT_EQ(map.find(1, 0), ref.at({1, 0}));
}

// ---------------------------------------------------------------------------- wire

TEST(ChunkWire, SectionOfEveryWidthDecodesToTheSameBlocks) {
    std::mt19937 rng(77);
    // 1 -> single value; 2..16 -> 4 bits; 17..32 -> 5; ...; 129..256 -> 8; more -> 16 raw.
    const std::pair<usize, u32> cases[] = {{1, 0},  {2, 4},  {3, 4},  {4, 4},   {5, 4},   {9, 4},   {16, 4},   {17, 5},  {32, 5},
                                           {33, 6}, {64, 6}, {65, 7}, {128, 7}, {129, 8}, {256, 8}, {257, 16}, {900, 16}};
    for (const auto& [palette, wire_bits] : cases) {
        SectionAllocator a;
        const auto states = some_states(palette, rng);
        std::vector<BlockState> ref(Section::kVolume);
        for (u32 i = 0; i < Section::kVolume; ++i) ref[i] = states[i < palette ? i : rng() % palette];  // every state present
        Section s;
        s.assign(a, ref.data());

        std::vector<u8> buf(section_wire_size(s, 39) + 64);
        const u8* end = put_section(buf.data(), s, 39);
        ASSERT_EQ(static_cast<usize>(end - buf.data()), section_wire_size(s, 39)) << palette;

        proto::Reader r(buf.data(), static_cast<usize>(end - buf.data()));
        EXPECT_EQ(r.be16(), s.non_air);
        EXPECT_EQ(r.be16(), s.fluids);  // 26.x: fluid count
        const Container c = read_container(r, 4096, 9);
        EXPECT_EQ(c.bits, wire_bits) << palette << " states";
        if (wire_bits >= 4 && wire_bits <= 8) {
            EXPECT_EQ(c.palette.size(), palette);
        }
        ASSERT_EQ(c.values.size(), 4096u);
        for (u32 i = 0; i < 4096; ++i) ASSERT_EQ(c.values[i], ref[i]) << palette << " states, index " << i;
        const Container biomes = read_container(r, 64, 4);
        EXPECT_EQ(biomes.bits, 0u);
        EXPECT_EQ(biomes.palette[0], 39u);
        EXPECT_TRUE(r.done());
        s.release(a);
    }
}

TEST(ChunkWire, SectionWrittenThroughSetIsTheSameOnTheWire) {
    // A palette built block by block has its states in a different slot order and may carry dead
    // slots; what the client decodes must not depend on that.
    std::mt19937 rng(8);
    for (usize palette : {3u, 6u, 20u, 70u, 200u}) {
        SectionAllocator a;
        const auto states = some_states(palette + 5, rng);
        std::vector<BlockState> ref(Section::kVolume, kAir);
        Section s;
        for (int n = 0; n < 20000; ++n) {
            const u32 i = rng() % Section::kVolume;
            s.set(a, i, ref[i] = states[rng() % states.size()]);
        }
        for (u32 i = 0; i < Section::kVolume; ++i)  // make five states disappear again
            if (std::find(states.end() - 5, states.end(), ref[i]) != states.end()) s.set(a, i, ref[i] = states[0]);
        std::vector<u8> buf(section_wire_size(s, 0) + 64);
        const u8* end = put_section(buf.data(), s, 0);
        proto::Reader r(buf.data(), static_cast<usize>(end - buf.data()));
        r.be16();
        r.be16();
        const Container c = read_container(r, 4096, 9);
        for (u32 i = 0; i < 4096; ++i) ASSERT_EQ(c.values[i], ref[i]);
        read_container(r, 64, 4);
        EXPECT_TRUE(r.done());
        s.release(a);
    }
}

TEST(ChunkWire, FlatChunkPacketLayout) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    const std::span<const u8> frame = dim.frame(3, -2);

    // Frame: 3-byte length, then the packet id.
    proto::Reader r(frame.data(), frame.size());
    std::span<const u8> body;
    u32 consumed = 0;
    ASSERT_EQ(proto::frame_split(frame.data(), frame.data() + frame.size(), proto::kMaxFrame, body, consumed),
              proto::FrameStatus::Ok);
    EXPECT_EQ(consumed, frame.size());
    r = proto::Reader(body);
    EXPECT_EQ(r.varint(), proto::ids::play::cb::kLevelChunkWithLight);
    EXPECT_EQ(static_cast<i32>(r.be32()), 3);
    EXPECT_EQ(static_cast<i32>(r.be32()), -2);

    // Heightmaps: the three client types, each 37 longs of 9-bit entries. Top block is the grass at
    // y = -61, so every column reads -61 + 1 - (-64) = 4.
    ASSERT_EQ(r.varint(), 3u);
    std::set<u32> types;
    for (int i = 0; i < 3; ++i) {
        types.insert(r.varint());
        const auto longs = read_long_array(r);
        ASSERT_EQ(longs.size(), 37u);
        for (u32 col = 0; col < 256; ++col) ASSERT_EQ((longs[col / 7] >> ((col % 7) * 9)) & 511, 4u);
        EXPECT_EQ(longs[36] >> 36, 0u);
    }
    EXPECT_EQ(types, (std::set<u32>{1, 4, 5}));  // WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES

    const auto data = r.byte_array(1u << 21);
    proto::Reader d(data);
    for (u32 si = 0; si < 24; ++si) {
        const u32 non_air = d.be16(), fluids = d.be16();
        const Container c = read_container(d, 4096, 9);
        const Container b = read_container(d, 64, 4);
        EXPECT_EQ(b.palette[0], 39u);
        EXPECT_EQ(fluids, 0u);
        if (si != 0) {
            EXPECT_EQ(non_air, 0u);
            EXPECT_EQ(c.bits, 0u);
            EXPECT_EQ(c.palette[0], kAir);
            continue;
        }
        EXPECT_EQ(non_air, 4u * 256);
        EXPECT_EQ(c.bits, 4u);  // four states: vanilla's smallest block palette
        for (u32 i = 0; i < 4096; ++i) {
            const u32 y = i >> 8;
            const u32 want = y == 0 ? blk::kBedrock : y <= 2 ? blk::kDirt : y == 3 ? blk::kGrassBlock : blk::kAir;
            ASSERT_EQ(c.values[i], want) << i;
        }
    }
    EXPECT_TRUE(d.done());
    EXPECT_EQ(r.varint(), 0u);  // block entities

    // Light. 26 light sections, index 0 is the one below the world. What a vanilla server holds
    // for this chunk (traced through SkyLightEngine.propagateLightSources):
    //   sky    0: stored but never written -> "empty" bit          block  0..2: stored, never written
    //          1: 0 below the surface, 15 from y = -60 up
    //          2: 15 everywhere
    //          3+: not stored (no block within a section of them)
    EXPECT_EQ(read_bit_set(r), 0b110u);  // sky
    EXPECT_EQ(read_bit_set(r), 0u);      // block
    EXPECT_EQ(read_bit_set(r), 0b001u);  // empty sky
    EXPECT_EQ(read_bit_set(r), 0b111u);  // empty block
    ASSERT_EQ(r.varint(), 2u);
    const auto low = r.byte_array(2048), high = r.byte_array(2048);
    ASSERT_EQ(low.size(), 2048u);
    ASSERT_EQ(high.size(), 2048u);
    for (u32 i = 0; i < 4096; ++i) {  // nibble index y<<8 | z<<4 | x, low nibble first
        const u32 y = i >> 8;
        ASSERT_EQ((low[i >> 1] >> ((i & 1) * 4)) & 15, y >= 4 ? 15u : 0u) << i;
        ASSERT_EQ((high[i >> 1] >> ((i & 1) * 4)) & 15, 15u);
    }
    EXPECT_EQ(r.varint(), 0u);  // block light arrays
    EXPECT_TRUE(r.done());
}

TEST(ChunkWire, FrameIsCachedUntilTheChunkChanges) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, 256);
    const std::span<const u8> a = dim.frame(0, 0);
    const std::vector<u8> first(a.begin(), a.end());
    // A view of radius 0 needs the 3x3 around it lit and the 5x5 generated.
    EXPECT_EQ(dim.chunk_count(), 25u);
    EXPECT_EQ(dim.find(0, 0)->status, ChunkStatus::Lit);
    EXPECT_TRUE(dim.find(0, 0)->ticking);
    EXPECT_EQ(dim.find(1, 1)->status, ChunkStatus::Lit);
    EXPECT_FALSE(dim.find(1, 1)->ticking);
    EXPECT_EQ(dim.find(2, 2)->status, ChunkStatus::LightReady);
    EXPECT_EQ(dim.find(3, 0), nullptr);

    const std::span<const u8> b = dim.frame(0, 0);
    EXPECT_EQ(a.data(), b.data());  // same cached bytes, not re-encoded
    EXPECT_LT(a.size(), 600u);      // compressed: a flat chunk is a few hundred bytes

    ASSERT_TRUE(dim.set_block(5, -60, 5, blk::kStone));
    EXPECT_FALSE(dim.find(0, 0)->frame_valid);
    const std::span<const u8> c = dim.frame(0, 0);
    EXPECT_NE(std::vector<u8>(c.begin(), c.end()), first);
    ASSERT_TRUE(dim.set_block(5, -60, 5, kAir));
    dim.update_light();
    dim.end_tick();
    const std::span<const u8> e = dim.frame(0, 0);
    EXPECT_EQ(std::vector<u8>(e.begin(), e.end()), first);  // back to the original world: identical packet
}

TEST(ChunkWire, LightUpdatePacketCarriesOnlyTheRequestedSections) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.frame(0, 0);
    base::ByteBuf out;
    write_light_update(out, proto::kNoCompression, *dim.find(0, 0), 0b010, 0b100);
    std::span<const u8> body;
    u32 consumed = 0;
    ASSERT_EQ(proto::frame_split(out.data, out.data + out.len, proto::kMaxFrame, body, consumed), proto::FrameStatus::Ok);
    proto::Reader r(body);
    EXPECT_EQ(r.varint(), proto::ids::play::cb::kLightUpdate);
    EXPECT_EQ(r.varint(), 0u);
    EXPECT_EQ(r.varint(), 0u);
    EXPECT_EQ(read_bit_set(r), 0b010u);  // sky: section 1 only
    EXPECT_EQ(read_bit_set(r), 0u);
    EXPECT_EQ(read_bit_set(r), 0u);
    EXPECT_EQ(read_bit_set(r), 0b100u);  // block: section 2, stored but empty
    EXPECT_EQ(r.varint(), 1u);
    EXPECT_EQ(r.byte_array(2048).size(), 2048u);
    EXPECT_EQ(r.varint(), 0u);
    EXPECT_TRUE(r.done());
}

TEST(Dimension, FlatGeneratorMatchesVanillaDefaults) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    EXPECT_EQ(gen.spawn_height(), -60);
    EXPECT_EQ(gen.sea_level(), -63);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    EXPECT_EQ(dim.min_y(), -64);
    EXPECT_EQ(dim.max_y(), 319);
    dim.load(-7, 12);
    for (i32 x : {-112, -97})
        for (i32 z : {192, 207}) {
            EXPECT_EQ(dim.block(x, -64, z), blk::kBedrock);
            EXPECT_EQ(dim.block(x, -63, z), blk::kDirt);
            EXPECT_EQ(dim.block(x, -62, z), blk::kDirt);
            EXPECT_EQ(dim.block(x, -61, z), blk::kGrassBlock);
            EXPECT_EQ(dim.block(x, -60, z), kAir);
            EXPECT_EQ(dim.block(x, 319, z), kAir);
            EXPECT_EQ(dim.block(x, 320, z), kAir);  // outside the build height
            EXPECT_EQ(dim.block(x, -65, z), kAir);
        }
    EXPECT_EQ(dim.block(0, -61, 0), kAir);  // chunk (0, 0) is not loaded
    const Chunk& c = *dim.find(-7, 12);
    EXPECT_EQ(c.sections[0].bits, 2);  // 4 states in 2 bits: 1 KiB instead of vanilla's 2
    EXPECT_EQ(c.sections[0].non_air, 1024);
    for (u32 si = 1; si < 24; ++si) EXPECT_TRUE(c.sections[si].uniform());
}

TEST(Dimension, HeightmapsFollowBlockChanges) {
    VoidGenerator gen;
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.frame(0, 0);
    Chunk& c = *dim.find(0, 0);
    std::mt19937 rng(21);
    const BlockState pool[] = {kAir,
                               kAir,
                               static_cast<BlockState>(blk::kStone),
                               static_cast<BlockState>(blk::kGlass),
                               static_cast<BlockState>(blk::kWater),
                               static_cast<BlockState>(blk::kOakLeaves),
                               static_cast<BlockState>(blk::kShortGrass),
                               static_cast<BlockState>(blk::kCaveAir)};
    // Brute force: highest y in the column whose state has the flag pattern, +1, relative to min_y.
    const auto expect = [&](u32 x, u32 z, u32 mask, u32 want) {
        for (i32 y = dim.max_y(); y >= dim.min_y(); --y)
            if ((flags_of(dim.block(static_cast<i32>(x), y, static_cast<i32>(z))) & mask) == want)
                return static_cast<u32>(y + 1 - dim.min_y());
        return 0u;
    };
    for (int round = 0; round < 40; ++round) {
        for (int n = 0; n < 60; ++n) {
            const i32 x = static_cast<i32>(rng() % 4), z = static_cast<i32>(rng() % 4);
            const i32 y = (rng() % 3 == 0) ? static_cast<i32>(rng() % 384) - 64 : static_cast<i32>(rng() % 12) + 60;
            dim.set_block(x, y, z, pool[rng() % std::size(pool)]);
        }
        for (u32 x = 0; x < 4; ++x)
            for (u32 z = 0; z < 4; ++z) {
                const u32 i = x + z * 16;
                ASSERT_EQ(c.world_surface.get(i), expect(x, z, state::kIsAir, 0)) << round;
                ASSERT_EQ(c.motion_blocking.get(i), expect(x, z, state::kMotionBlocking, state::kMotionBlocking)) << round;
                ASSERT_EQ(c.motion_blocking_no_leaves.get(i),
                          expect(x, z, state::kMotionBlockingNoLeaves, state::kMotionBlockingNoLeaves))
                    << round;
            }
        dim.update_light();
        dim.end_tick();
    }
    // The predicates really differ: leaves block motion but not in the NO_LEAVES map, grass blocks neither.
    EXPECT_TRUE(flags_of(blk::kOakLeaves) & state::kMotionBlocking);
    EXPECT_FALSE(flags_of(blk::kOakLeaves) & state::kMotionBlockingNoLeaves);
    EXPECT_FALSE(flags_of(blk::kShortGrass) & state::kMotionBlocking);
    EXPECT_TRUE(flags_of(blk::kWater) & state::kMotionBlocking);  // fluids count
    EXPECT_TRUE(is_air(blk::kCaveAir));
}

TEST(Dimension, BlockChangesAreSortedGroupedAndDeduplicated) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.frame(0, 0);
    dim.frame(1, 0);
    EXPECT_FALSE(dim.set_block(40, -60, 0, blk::kStone));  // chunk (2, 0) exists but is not ticking
    EXPECT_FALSE(dim.set_block(0, 320, 0, blk::kStone));   // above the build height
    EXPECT_FALSE(dim.set_block(0, -65, 0, blk::kStone));
    EXPECT_FALSE(dim.set_block(0, -61, 0, blk::kGrassBlock));  // already that state
    EXPECT_FALSE(dim.set_block(0, 100, 0, kAir));
    EXPECT_TRUE(dim.set_block(17, -60, 3, blk::kStone));
    EXPECT_TRUE(dim.set_block(2, -60, 3, blk::kStone));
    EXPECT_TRUE(dim.set_block(2, -60, 3, blk::kDirt));  // same position twice in one tick
    EXPECT_TRUE(dim.set_block(2, 100, 3, blk::kStone));
    const auto& ch = dim.block_changes();
    ASSERT_EQ(ch.size(), 3u);
    EXPECT_EQ(ch[0].chunk, dim.find(0, 0));
    EXPECT_EQ(ch[0].section, 0);
    EXPECT_EQ(ch[0].pos, (2 << 8) | (3 << 4) | 4);  // x<<8 | z<<4 | y, y = -60 & 15
    EXPECT_EQ(ch[1].chunk, dim.find(0, 0));
    EXPECT_EQ(ch[1].section, 10);
    EXPECT_EQ(ch[2].chunk, dim.find(1, 0));
    dim.update_light();
    dim.end_tick();
    EXPECT_TRUE(dim.block_changes().empty());
    EXPECT_TRUE(dim.find(0, 0)->modified);
}

TEST(Dimension, SnowOnGrassMakesItSnowy) {
    // SnowyBlock.updateShape: the one neighbour reaction among the blocks a player can place.
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.frame(0, 0);
    const BlockInfo& grass = block_info_of(blk::kGrassBlock);
    ASSERT_EQ(grass.flags & block_flag::kPlaceMask, block_flag::kPlaceSnowy);
    const BlockState plain = grass.alt[0], snowy = grass.alt[1];
    ASSERT_EQ(plain, blk::kGrassBlock);
    ASSERT_NE(snowy, plain);
    EXPECT_TRUE(dim.set_block(4, -60, 4, blk::kSnowBlock));
    EXPECT_EQ(dim.block(4, -61, 4), snowy);
    EXPECT_EQ(dim.block_changes().size(), 2u);  // both go to the viewers
    EXPECT_TRUE(dim.set_block(4, -60, 4, blk::kStone));
    EXPECT_EQ(dim.block(4, -61, 4), plain);
    EXPECT_TRUE(dim.set_block(4, -60, 4, blk::kSnowBlock));
    EXPECT_TRUE(dim.set_block(4, -60, 4, kAir));
    EXPECT_EQ(dim.block(4, -61, 4), plain);
    EXPECT_TRUE(dim.set_block(5, -60, 4, blk::kStone));  // nothing snowy about stone
    EXPECT_EQ(dim.block(5, -61, 4), plain);
}

TEST(Blocks, GeneratedTablesAgreeWithEachOther) {
    EXPECT_TRUE(is_air(kAir));
    EXPECT_TRUE(is_air(blk::kVoidAir));
    EXPECT_FALSE(is_air(blk::kStone));
    EXPECT_EQ(opacity(kAir), 1u);  // light always loses a level per block
    EXPECT_EQ(dampening(kAir), 0u);
    EXPECT_EQ(opacity(blk::kStone), 15u);
    EXPECT_EQ(emission(blk::kGlowstone), 15u);
    EXPECT_EQ(dampening(blk::kGlass), 0u);
    EXPECT_TRUE(flags_of(blk::kWater) & state::kHasFluid);
    EXPECT_TRUE(is_replaceable(blk::kWater));
    EXPECT_FALSE(is_replaceable(blk::kStone));
    // A full cube without a shape of its own never occludes by shape; a bottom slab closes its
    // bottom face and half of each side.
    EXPECT_TRUE(has_empty_shape(blk::kStone));
    EXPECT_FALSE(has_empty_shape(blk::kOakSlab));
    EXPECT_TRUE(shape_occludes(kAir, blk::kOakSlab, kUp));     // entering a bottom slab from below
    EXPECT_FALSE(shape_occludes(kAir, blk::kOakSlab, kDown));  // from above: its top half is open
    EXPECT_FALSE(shape_occludes(kAir, kAir, kNorth));
    for (u32 b = 0; b < state::kBlockCount; ++b) {
        const BlockInfo& info = block_info(b);
        ASSERT_EQ(block_of(info.default_state), b);
        ASSERT_EQ(block_of(info.first_state), b);
        ASSERT_EQ(block_of(static_cast<BlockState>(info.first_state + info.state_count - 1)), b);
        ASSERT_LT(info.item, item_count());
        const u32 kind = info.flags & block_flag::kPlaceMask;
        if (kind == block_flag::kPlaceNone) continue;
        // Placeable: its item is a plain block item that places exactly this block.
        ASSERT_EQ(item_info(info.item).block, b) << block_name(b);
        ASSERT_TRUE(item_info(info.item).flags & item_flag::kPlainBlockItem) << block_name(b);
        ASSERT_FALSE(info.flags & (block_flag::kGameMaster | block_flag::kCustomReplace)) << block_name(b);
        // A block that answers a click, or keeps a block entity, is placeable only as a ported class.
        if ((info.flags & block_flag::kInteractive) || info.block_entity != proto::gen::kNoBlockEntity) {
            ASSERT_NE(info.kind, proto::gen::block_kind::kPlain) << block_name(b);
        }
        // What an explosion leaves of it is an item, or nothing.
        ASSERT_LT(info.drop[0].item, item_count());
        ASSERT_LE(info.drop[0].min, info.drop[0].max);
        ASSERT_TRUE(info.drop_chance > 0.0f && info.drop_chance <= 1.0f) << block_name(b);
        if (kind == block_flag::kPlaceAxis) {
            for (u32 k = 0; k < 3; ++k) ASSERT_EQ(block_of(info.alt[k]), b);
        }
    }
    EXPECT_EQ(block_name(block_of(blk::kStone)), "minecraft:stone");
    bool dynamic = true;
    const auto cube = collision_boxes(blk::kStone, dynamic);
    ASSERT_EQ(cube.size(), 1u);
    EXPECT_FALSE(dynamic);
    EXPECT_EQ(cube[0].min[1], 0.0);
    EXPECT_EQ(cube[0].max[1], 1.0);
    EXPECT_TRUE(collision_boxes(kAir, dynamic).empty());
    ASSERT_EQ(collision_boxes(blk::kOakSlab, dynamic).size(), 1u);
    EXPECT_EQ(collision_boxes(blk::kOakSlab, dynamic)[0].max[1], 0.5);
}

// ---------------------------------------------------------------------------- boxes against blocks

TEST(Collide, ABoxAgainstTheGroundAndAWall) {
    // The flat world's top block is grass at y = -61: the ground is the plane y = -60.
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.ticking(0, 0);
    std::vector<Aabb> scratch;
    double out[3];
    // A box the size of a primed TNT, half a block up, falling a whole block: it stops on the ground,
    // and keeps what it moved sideways.
    const Aabb box{{8.01, -59.5, 8.01}, {8.99, -58.52, 8.99}};
    const double fall[3] = {0.1, -1.0, -0.2};
    collide(dim, box, fall, out, scratch);
    EXPECT_DOUBLE_EQ(out[0], 0.1);
    EXPECT_DOUBLE_EQ(out[1], -0.5);
    EXPECT_DOUBLE_EQ(out[2], -0.2);
    // Resting on it, gravity moves it nowhere; in the air nothing stops it.
    const Aabb resting{{8.01, -60.0, 8.01}, {8.99, -59.02, 8.99}};
    const double gravity[3] = {0.0, -0.04, 0.0}, up[3] = {0.0, 0.16, 0.0};
    collide(dim, resting, gravity, out, scratch);
    EXPECT_EQ(out[1], 0.0);
    collide(dim, resting, up, out, scratch);
    EXPECT_EQ(out[1], 0.16);
    // A wall two blocks to the east: one block and a hundredth of free way.
    ASSERT_TRUE(dim.set_block(10, -60, 8, static_cast<BlockState>(blk::kStone)));
    const double east[3] = {2.0, 0.0, 0.0};
    collide(dim, resting, east, out, scratch);
    EXPECT_NEAR(out[0], 1.01, 1e-12);
    // ...which a box one block higher passes over.
    Aabb above = resting;
    above.min[1] += 1.0, above.max[1] += 1.0;
    collide(dim, above, east, out, scratch);
    EXPECT_EQ(out[0], 2.0);

    // What it stands on: the block under its middle - and, hanging over an edge, the nearest one.
    Aabb feet = resting;
    feet.max[1] = feet.min[1];
    feet.min[1] -= 1.0e-6;
    i32 pos[3];
    ASSERT_TRUE(supporting_block(dim, feet, 8.5, -60.0, 8.5, pos));
    EXPECT_EQ(pos[0], 8);
    EXPECT_EQ(pos[1], -61);
    EXPECT_EQ(pos[2], 8);
    Aabb over_edge = feet;
    over_edge.min[0] += 0.4, over_edge.max[0] += 0.4;  // x 8.41 .. 9.39: two blocks under it
    ASSERT_TRUE(supporting_block(dim, over_edge, 8.9, -60.0, 8.5, pos));
    EXPECT_EQ(pos[0], 8);  // its middle is still over block 8
    ASSERT_TRUE(supporting_block(dim, over_edge, 9.1, -60.0, 8.5, pos));
    EXPECT_EQ(pos[0], 9);
    Aabb flying = feet;
    flying.min[1] += 1.0, flying.max[1] += 1.0;
    EXPECT_FALSE(supporting_block(dim, flying, 8.5, -59.0, 8.5, pos));

    // Lines of sight: through the air, into the ground, through the wall, and from inside a block.
    const double eye[3] = {8.5, -58.4, 8.5}, far[3] = {14.5, -58.4, 11.5}, below[3] = {8.5, -62.5, 8.5};
    const double behind[3] = {11.5, -59.5, 8.5}, low[3] = {8.5, -59.5, 8.5}, inside[3] = {10.5, -59.5, 8.5};
    EXPECT_FALSE(clip_hits(dim, eye, far));
    EXPECT_TRUE(clip_hits(dim, eye, below));
    EXPECT_TRUE(clip_hits(dim, low, behind));
    const double over[3] = {11.5, -58.9, 8.5};
    EXPECT_FALSE(clip_hits(dim, eye, over));   // above the wall's top
    EXPECT_TRUE(clip_hits(dim, eye, behind));  // down through its top face
    EXPECT_TRUE(clip_hits(dim, inside, eye));
    EXPECT_FALSE(clip_hits(dim, eye, eye));
}

TEST(Collide, TheRaysOfAnExplosion) {
    // A TNT's worth (radius 4) on flat ground: grass and dirt give way (resistance 0.6 and 0.5),
    // the bedrock under them stops every ray, and nothing beyond seven blocks is reached.
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.ticking(0, 0);
    float strengths[kBlastRays];
    for (float& s : strengths) s = 0.5f;  // every ray starts with radius * 1.0
    BlastScratch scratch;
    std::vector<BlockAt> out;
    blast_positions(dim, 8.5, -59.93875, 8.5, 4.0f, strengths, scratch, out);
    std::set<std::tuple<i32, i32, i32>> seen;
    u32 solid = 0;
    for (const BlockAt& b : out) {
        EXPECT_TRUE(seen.insert({b.x, b.y, b.z}).second) << "a position twice";
        EXPECT_LE(std::abs(b.x - 8), 7);
        EXPECT_LE(std::abs(b.y + 60), 7);
        EXPECT_GT(b.y, -64) << "bedrock";
        solid += !is_air(dim.block(b.x, b.y, b.z));
    }
    EXPECT_TRUE(seen.count({8, -61, 8}));   // the grass it lies on
    EXPECT_TRUE(seen.count({8, -63, 8}));   // ...and the dirt down to the bedrock
    EXPECT_TRUE(seen.count({8, -57, 8}));   // air counts too
    EXPECT_FALSE(seen.count({8, -52, 8}));  // 4 / 0.225 * 0.3 = 5.3 blocks through the air at most
    EXPECT_GT(solid, 40u);
    EXPECT_LT(solid, 400u);
    // Twice the strength reaches further; walls and a roof of obsidian around the centre let
    // nothing through above the ground - under them the rays still dig.
    for (float& s : strengths) s = 1.0f;
    std::vector<BlockAt> more;
    blast_positions(dim, 8.5, -59.93875, 8.5, 4.0f, strengths, scratch, more);
    EXPECT_GT(more.size(), out.size());
    for (i32 x = 6; x <= 10; ++x) {
        for (i32 y = -60; y <= -57; ++y) {
            for (i32 z = 6; z <= 10; ++z) {
                if (x == 6 || x == 10 || z == 6 || z == 10 || y == -57) {
                    ASSERT_TRUE(dim.set_block(x, y, z, static_cast<BlockState>(blk::kObsidian)));
                }
            }
        }
    }
    blast_positions(dim, 8.5, -59.93875, 8.5, 4.0f, strengths, scratch, more);
    u32 below = 0;
    for (const BlockAt& b : more) {
        if (b.y < -60) {
            ++below;
            continue;
        }
        EXPECT_TRUE(b.x > 6 && b.x < 10 && b.z > 6 && b.z < 10 && b.y < -57) << b.x << ' ' << b.y << ' ' << b.z;
    }
    EXPECT_GT(below, 20u);
}
