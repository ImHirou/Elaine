// Chunk -> wire. Layouts read from the 26.3 jar (LevelChunkSection.write, PalettedContainer$Data.write,
// Strategy, SimpleBitStorage, ClientboundLevelChunkPacketData, ClientboundLightUpdatePacketData):
//
//   section        i16 non-air count, i16 fluid count (26.x), block states, biomes
//   biomes         the same container over the 64 cells of a section (4x4x4): bits 0 one biome,
//                  1..3 a palette, otherwise ceillog2 of the biome count and raw ids
//   container      u8 bits | palette | longs WITHOUT a count
//                    bits 0        single value: VarInt id, no longs
//                    bits 4..8     VarInt n + n VarInt ids, then ceil(4096 / (64/bits)) longs
//                                  (blocks never use 1..3: the client reads those as 4)
//                    bits 16       no palette, 1024 longs of raw ids (ceillog2 of the state count)
//                  entries fill each long from its low bits and never span two longs
//   chunk          i32 x, i32 z | VarInt 3 x (VarInt type, VarInt 37, 37 longs) | VarInt size + sections
//                  | VarInt 0 block entities | light
//   light          4 bit sets (sky, block, empty sky, empty block; ByteBufCodecs.BIT_SET = VarInt
//                  byte count + little-endian bytes, not longs), then VarInt count + per set bit
//                  VarInt 2048 + 2048 bytes, sky arrays first. Bit i = light section i, which
//                  starts one section below the world.
//
// The in-memory section uses exact widths in a dense layout; put_section converts with pdep/pext or,
// where the two layouts coincide (4, 8 and 16 bits), a byte-swapping copy.
#pragma once

#include "base/buf.hpp"
#include "world/chunk.hpp"

namespace world {

// `biomes`: the section's 64 cells when they are not all `biome`.
usize section_wire_size(const Section& s, u32 biome, const BiomeSection* biomes = nullptr);  // exactly what put_section writes
u8* put_section(u8* p, const Section& s, u32 biome, const BiomeSection* biomes = nullptr);

usize chunk_wire_bound(const Chunk& c);
u8* put_chunk(u8* p, const Chunk& c);  // level_chunk_with_light payload
// Light data for the light sections named in the two masks (all of them for a chunk packet).
usize light_wire_bound(const Chunk& c, u32 sky_mask, u32 block_mask);
u8* put_light(u8* p, const Chunk& c, u32 sky_mask, u32 block_mask);

// Whole packets, framed (and compressed past the threshold) onto `out`.
void write_chunk_packet(base::ByteBuf& out, i32 threshold, const Chunk& c);
void write_light_update(base::ByteBuf& out, i32 threshold, const Chunk& c, u32 sky_mask, u32 block_mask);

}  // namespace world
