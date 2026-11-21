// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/chunk_codec.hpp"

#include "proto/frame.hpp"
#include "proto/wire.hpp"

namespace world {

using namespace proto;

namespace {

// Strategy.globalPaletteBitsInMemory = Mth.ceillog2(number of block states).
constexpr u32 kDirectWireBits = static_cast<u32>(std::bit_width(state::kCount - 1));
// Direct sections are u16 arrays and go out as a byte-swapping copy. A registry that fits 15 bits
// (or needs 17) changes the wire width: write the repack then.
static_assert(kDirectWireBits == Section::kDirectBits);

// Blocks: a palette of up to 16 states is always 4 bits wide on the wire.
ALWAYS_INLINE u32 wire_bits(const Section& s) { return std::max<u32>(4, s.bits); }

u8* put_block_states(u8* p, const Section& s) {
    if (s.bits == 0) {
        p = put_u8(p, 0);
        return put_varint(p, s.single);
    }
    if (s.bits == Section::kDirectBits) {
        p = put_u8(p, kDirectWireBits);
        for (u32 j = 0; j < kSectionVolume / 4; ++j) p = put_be64(p, load_le<u64>(s.data + 8 * j));
        return p;
    }
    const u32 bits = s.bits;
    p = put_u8(p, static_cast<u8>(wire_bits(s)));
    p = put_varint(p, s.palette_len);
    for (u32 i = 0; i < s.palette_len; ++i) p = put_varint(p, s.palette[i]);

    if (bits < 4) {
        // 16 entries are 2*bits bytes in memory and one nibble-per-entry long on the wire.
        const u64 spread = 0x1111111111111111ull * ((1u << bits) - 1);
        for (u32 j = 0; j < kSectionVolume / 16; ++j) p = put_be64(p, _pdep_u64(load_le<u64>(s.data + 2 * bits * j), spread));
    } else if (bits == 4 || bits == 8) {
        // 64 / bits entries per long with no padding: the dense layout IS the wire layout.
        for (u32 j = 0; j < packed_bytes(bits) / 8; ++j) p = put_be64(p, load_le<u64>(s.data + 8 * j));
    } else {
        // 5, 6, 7 bits: 12, 10, 9 entries per long, the rest of the long is padding.
        alignas(64) u8 idx[kSectionVolume + 16];
        unpack_indices(s.data, bits, idx);
        std::memset(idx + kSectionVolume, 0, 16);
        const u32 per = 64 / bits;
        const u64 keep = 0x0101010101010101ull * ((1u << bits) - 1);
        const u64 tail = (1ull << (8 * (per - 8))) - 1;  // the entries past the first eight
        for (u32 j = 0; j < words_for(kSectionVolume, bits); ++j) {
            const u8* e = idx + j * per;
            const u64 lo = _pext_u64(load_le<u64>(e), keep);
            const u64 hi = _pext_u64(load_le<u64>(e + 8) & tail, keep);
            p = put_be64(p, lo | (hi << (8 * bits)));
        }
    }
    return p;
}

usize block_states_size(const Section& s) {
    if (s.bits == 0) return 1 + varint_size(s.single);
    if (s.bits == Section::kDirectBits) return 1 + kSectionVolume * 2;
    usize n = 1 + varint_size(s.palette_len) + 8 * words_for(kSectionVolume, wire_bits(s));
    for (u32 i = 0; i < s.palette_len; ++i) n += varint_size(s.palette[i]);
    return n;
}

u8* put_layers(u8* p, const Chunk& c, bool sky, u64 mask) {
    p = put_varint(p, static_cast<u32>(std::popcount(mask)));
    for (u64 m = mask; m; m &= m - 1) {
        const u32 li = static_cast<u32>(std::countr_zero(m));
        const LightLayer& l = sky ? c.sky_layer(li) : c.block_layer(li);
        p = put_varint(p, LightLayer::kBytes);
        if (l.has_array()) {
            std::memcpy(p, l.array(), LightLayer::kBytes);
        } else {
            const u32 v = l.uniform_value();
            std::memset(p, static_cast<int>(v | v << 4), LightLayer::kBytes);
        }
        p += LightLayer::kBytes;
    }
    return p;
}

u8* put_column_map(u8* p, u32 type, const ColumnMap& map) {
    p = put_varint(p, type);
    p = put_varint(p, kHeightmapWords);
    for (u64 w : map.w) p = put_be64(p, w);
    return p;
}

// ClientboundLightUpdatePacketData.prepareSectionData for every requested section of one layer.
void classify(const Chunk& c, bool sky, u32 filter, u64& data, u64& empty) {
    for (u32 li = 0; li < c.light_count; ++li) {
        const LightLayer& l = sky ? c.sky_layer(li) : c.block_layer(li);
        if (!((filter >> li) & 1) || !l.present()) continue;
        (l.vanilla_empty() ? empty : data) |= 1ull << li;
    }
}

// Strategy.createForBiomes: one biome is no bits; up to eight are a palette of 1, 2 or 3 bits; more
// are the registry's ids at their full width. 64 cells in vanilla's word layout.
constexpr u32 biome_wire_bits(u32 count) {
    const u32 bits = static_cast<u32>(std::bit_width(count - 1));
    return bits <= 3 ? bits : static_cast<u32>(std::bit_width(proto::gen::reg::worldgen_biome::kCount - 1));
}

u8* put_biomes(u8* p, u32 biome, const BiomeSection* b) {
    if (!b || b->count == 0) {
        p = put_u8(p, 0);
        return put_varint(p, biome);
    }
    const u32 bits = biome_wire_bits(b->count);
    const bool direct = bits > 3;
    p = put_u8(p, static_cast<u8>(bits));
    if (!direct) {
        p = put_varint(p, b->count);
        for (u32 i = 0; i < b->count; ++i) p = put_varint(p, b->palette[i]);
    }
    u64 words[64] = {};
    for (u32 i = 0; i < 64; ++i) words_set(words, bits, i, direct ? b->palette[b->index[i]] : b->index[i]);
    for (u32 j = 0; j < words_for(64, bits); ++j) p = put_be64(p, words[j]);
    return p;
}

usize biomes_size(u32 biome, const BiomeSection* b) {
    if (!b || b->count == 0) return 1 + varint_size(biome);
    const u32 bits = biome_wire_bits(b->count);
    usize n = 1 + 8 * words_for(64, bits);
    if (bits <= 3) {
        n += varint_size(b->count);
        for (u32 i = 0; i < b->count; ++i) n += varint_size(b->palette[i]);
    }
    return n;
}

}  // namespace

usize section_wire_size(const Section& s, u32 biome, const BiomeSection* biomes) {
    return 4 + block_states_size(s) + biomes_size(biome, biomes);
}

u8* put_section(u8* p, const Section& s, u32 biome, const BiomeSection* biomes) {
    p = put_be16(p, s.non_air);
    p = put_be16(p, s.fluids);
    p = put_block_states(p, s);
    return put_biomes(p, biome, biomes);
}

usize light_wire_bound(const Chunk& c, u32 sky_mask, u32 block_mask) {
    u64 sky = 0, block = 0, es = 0, eb = 0;
    classify(c, true, sky_mask, sky, es);
    classify(c, false, block_mask, block, eb);
    return 4 * kBitSetMaxBytes + 2 * kVarIntMaxBytes +
           static_cast<usize>(std::popcount(sky) + std::popcount(block)) * (2 + LightLayer::kBytes);
}

u8* put_light(u8* p, const Chunk& c, u32 sky_mask, u32 block_mask) {
    u64 sky = 0, block = 0, empty_sky = 0, empty_block = 0;
    classify(c, true, sky_mask, sky, empty_sky);
    classify(c, false, block_mask, block, empty_block);
    p = put_bit_set(p, sky);
    p = put_bit_set(p, block);
    p = put_bit_set(p, empty_sky);
    p = put_bit_set(p, empty_block);
    p = put_layers(p, c, true, sky);
    return put_layers(p, c, false, block);
}

usize chunk_wire_bound(const Chunk& c) {
    usize n = 8 + 1 + 3 * (2 + 2 + 8 * kHeightmapWords) + 5 + kVarIntMaxBytes +
              static_cast<usize>(c.block_entity_count) * (1 + 2 + kVarIntMaxBytes + 1);
    for (u32 si = 0; si < c.sections_count; ++si)
        n += section_wire_size(c.sections[si], c.biome[si], c.biomes ? &c.biomes[si] : nullptr);
    return n + light_wire_bound(c, ~0u, ~0u);
}

u8* put_chunk(u8* p, const Chunk& c) {
    p = put_be32(p, static_cast<u32>(c.x));
    p = put_be32(p, static_cast<u32>(c.z));
    // The three Heightmap.Types with Usage.CLIENT. It is a map on the wire: the order is free.
    p = put_varint(p, 3);
    p = put_column_map(p, kWorldSurface, c.world_surface);
    p = put_column_map(p, kMotionBlocking, c.motion_blocking);
    p = put_column_map(p, kMotionBlockingNoLeaves, c.motion_blocking_no_leaves);

    usize size = 0;
    const auto biomes_of = [&](u32 si) { return c.biomes ? &c.biomes[si] : nullptr; };
    for (u32 si = 0; si < c.sections_count; ++si) size += section_wire_size(c.sections[si], c.biome[si], biomes_of(si));
    p = put_varint(p, static_cast<u32>(size));
    [[maybe_unused]] const u8* start = p;
    for (u32 si = 0; si < c.sections_count; ++si) p = put_section(p, c.sections[si], c.biome[si], biomes_of(si));
    DCHECK(static_cast<usize>(p - start) == size);

    // ClientboundLevelChunkPacketData.BlockEntityInfo: packed x/z, y, type, and the block entity's
    // update tag - BlockEntity.getUpdateTag(), which is empty for every type the server has (a
    // chest and a furnace keep their contents to themselves), and an empty tag travels as "no
    // tag": one TAG_End (FriendlyByteBuf.writeNbt(null)).
    p = put_varint(p, c.block_entity_count);
    for (u32 i = 0; i < c.block_entity_count; ++i) {
        const BlockEntityRef& be = c.block_entities[i];
        p = put_u8(p, be.xz);
        p = put_be16(p, static_cast<u16>(be.y));
        p = put_varint(p, be.type);
        p = put_u8(p, 0);
    }
    return put_light(p, c, ~0u, ~0u);
}

void write_chunk_packet(base::ByteBuf& out, i32 threshold, const Chunk& c) {
    FrameMark m;
    u8* p = frame_begin(out, threshold, chunk_wire_bound(c) + kVarIntMaxBytes, m);
    p = put_varint(p, ids::play::cb::kLevelChunkWithLight);
    p = put_chunk(p, c);
    frame_end(out, threshold, m, p);
}

void write_light_update(base::ByteBuf& out, i32 threshold, const Chunk& c, u32 sky_mask, u32 block_mask) {
    FrameMark m;
    u8* p = frame_begin(out, threshold, light_wire_bound(c, sky_mask, block_mask) + 3 * kVarIntMaxBytes, m);
    p = put_varint(p, ids::play::cb::kLightUpdate);
    p = put_varint(p, static_cast<u32>(c.x));
    p = put_varint(p, static_cast<u32>(c.z));
    p = put_light(p, c, sky_mask, block_mask);
    frame_end(out, threshold, m, p);
}

}  // namespace world
