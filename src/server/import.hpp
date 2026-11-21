// SPDX-License-Identifier: GPL-3.0-or-later
// Import of a vanilla world: every chunk of its Anvil region files becomes a chunk of this
// server's own (world/chunk_io.hpp), once, before the server starts.
//
// Only a world of this version's data version is read; an older one is upgraded by vanilla
// first. Layouts are the 26.3 jar's (SerializableChunkData, PalettedContainer, BlockState.CODEC,
// RegionFile, EntityStorage, BlockEntity.saveWithFullMetadata, ContainerHelper):
//
//   region/r.X.Z.mca     8 KiB of locations and timestamps, then chunks in 4 KiB sectors: i32
//                        length, u8 compression (1 gzip, 2 zlib, 3 none; +128: the bytes are in
//                        c.X.Z.mcc beside it), a named compound
//   a chunk              DataVersion, Status, xPos, zPos, isLightOn, sections [Y, block_states
//                        {palette, data}, biomes {palette, data}, SkyLight, BlockLight],
//                        block_entities [...] and what nobody here reads
//   a block state        26.x: its block's name alone for the default state, else
//                        {id, properties: {name: value...}} (it was {Name, Properties})
//   entities/r.X.Z.mca   the same file format; a chunk is {Position [I; x, z], Entities [...]}
//
// What becomes of it:
//   blocks, biomes   as they are. A chunk vanilla had not finished generating (Status other than
//                    minecraft:full) holds nothing anyone made: it is left to the generator here.
//   light            the layers that were saved, queued as vanilla queues them when it loads the
//                    chunk; a chunk saved lit is not lit again.
//   block entities   a chest or a furnace whose tag says nothing this server would not act on -
//                    stacks without components, no name, lock or loot table - becomes one of ours.
//                    Every other block entity is kept as its tag (canonical: nbt::Doc::write): a
//                    client is told that it is there, and it does nothing.
//   entities         kept as their tags, with the chunk they were saved in. None is in the world:
//                    there is no reader for an entity's saved form yet.
//   the rest         every other entry of the chunk's tag (structures, ticks that were scheduled,
//                    InhabitedTime...) is kept with the chunk. Heightmaps are not: they follow
//                    from the blocks.
// Nothing is dropped that a later export could not write back: what is not read is kept, and what
// is neither is derived (world::KeptData):
//
//   kept := u8 1, u32 n + the chunk's other entries as one compound, u32 count, count x (u32 n +
//           an entity's compound) - compounds in network form (type byte, no name), numbers
//           little-endian.
//
// The vanilla files are opened for reading and never written.
#pragma once

#include <string>

#include "base/buf.hpp"
#include "base/workers.hpp"
#include "nbt/nbt.hpp"
#include "server/world_info.hpp"
#include "world/chunk.hpp"

namespace server {

struct ImportStats {
    u64 chunks = 0;          // imported with their blocks
    u64 unfinished = 0;      // left to the generator: vanilla had not finished them
    u64 block_entities = 0;  // made ours (chests, furnaces)
    u64 kept_block_entities = 0;
    u64 entities = 0;  // kept as tags
    void add(const ImportStats& o) {
        chunks += o.chunks, unfinished += o.unfinished, block_entities += o.block_entities;
        kept_block_entities += o.kept_block_entities, entities += o.entities;
    }
};

// One chunk. `terrain`: the root tag of its entry in region/, or no tag; `entities`: the same of
// entities/. Appends the chunk's bytes to `out` - nothing, when there is nothing to keep of it.
// False with `err` for a chunk that is not of this version or names what does not exist.
bool import_chunk(nbt::Doc& terrain_doc, nbt::Doc& entities_doc, i32 cx, i32 cz, const world::Layout& layout, base::ByteBuf& out,
                  ImportStats& stats, std::string& err);

// One region: `mca` and `entities_mca` are the two vanilla files of it (either may be missing),
// `out` the file of ours to make.
bool import_region(const std::string& mca, const std::string& entities_mca, i32 rx, i32 rz, const std::string& out,
                   const world::Layout& layout, ImportStats& stats, std::string& err);

// The vanilla world in `dir` (which has a level.dat): its overworld's chunks into
// `info.chunk_dir`, its generator and spawn into `info`.
bool import_world(const std::string& dir, base::Workers& workers, WorldInfo& info, std::string& err);

}  // namespace server
