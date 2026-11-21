// SPDX-License-Identifier: GPL-3.0-or-later
// Region files: where chunks are on disk. One file per 32 x 32 chunks, as in Anvil - and nothing
// else as in Anvil.
//
//   [0, 16)        "MCRGN\0\0\1", u32 sector size (512), u32 0
//   [16, 16400)    1024 entries of 16 bytes, entry = local x + local z * 32:
//                    u32 sector     first sector of the chunk's bytes, 0 = no chunk
//                    u32 bytes      what is stored (deflated)
//                    u32 raw_bytes  what it inflates to
//                    u32 crc32      of the stored bytes
//   from sector 33 chunks, each a run of whole sectors. All numbers little-endian.
//
// What that buys over Anvil's 4 KiB sectors and one-byte lengths: a chunk of a flat world - some
// seventy bytes - takes half a kilobyte, not four; a chunk can be any size (no side files); a torn
// or misdirected write is found by its checksum instead of being parsed.
//
// A chunk is never overwritten in place. Its new bytes go to free sectors, the file is synced, the
// header entry is switched (16 bytes inside one disk sector), the file is synced again, and only
// then are the old sectors free to be used. Whenever the process or the machine stops, every entry
// points at a whole chunk: the new one or the one before.
//
// Nothing here is shared between threads. A RegionFile belongs to whoever writes the region - one
// write job at a time, handed over and back by the tick thread (world/store.hpp); readers need only
// the file descriptor and a copy of an entry (pread).
#pragma once

#include <span>
#include <string>
#include <vector>

#include "base/types.hpp"

namespace world {

inline constexpr u32 kRegionChunks = 32 * 32;
inline constexpr u32 kRegionSector = 512;

struct RegionEntry {
    u32 sector = 0;
    u32 bytes = 0;
    u32 raw_bytes = 0;
    u32 crc = 0;
};
static_assert(sizeof(RegionEntry) == 16);

struct RegionFile {
    int fd = -1;
    RegionEntry table[kRegionChunks];
    std::vector<u64> used;  // a bit per sector: taken by the header or by a chunk an entry points at
    u32 sectors = 0;        // sectors the bitmap covers

    RegionFile() = default;
    RegionFile(const RegionFile&) = delete;
    RegionFile& operator=(const RegionFile&) = delete;
    ~RegionFile();

    // Open `path`; with `create`, make an empty region if there is no file. False with `err` set
    // when it cannot be opened or is not a region file. A missing file without `create`: false,
    // err empty.
    bool open(const std::string& path, bool create, std::string& err);
    void close();

    // One chunk to store: `raw` is deflated and written to free sectors.
    struct Put {
        u32 index;  // local x + local z * 32
        std::span<const u8> raw;
    };
    // Store a batch as described at the top: data, sync, entries, sync; then the sectors of what
    // the entries pointed at before are free. `sync` = false leaves the syncing to the kernel
    // (a crash of the machine may then lose the batch, not more). False on an I/O error: the
    // table is as it was for the chunks that did not make it.
    bool write(std::span<const Put> batch, bool sync, std::string& err);
};

// Read and inflate the chunk an entry points at. False (with `err`) if the bytes are not what the
// entry says - a short read, a checksum that does not match, a stream that does not inflate.
bool region_read(int fd, const RegionEntry& e, std::vector<u8>& raw, std::string& err);

// Deflate / inflate with the calling thread's own (de)compressor.
void deflate_bytes(std::span<const u8> raw, std::vector<u8>& out);
bool inflate_bytes(std::span<const u8> stored, u8* raw, usize raw_bytes);
// zlib and gzip streams of unknown size (Anvil chunks, level.dat). False if it is not one.
bool inflate_zlib(std::span<const u8> stored, std::vector<u8>& out);
bool inflate_gzip(std::span<const u8> stored, std::vector<u8>& out);
u32 crc32_of(std::span<const u8> bytes);

}  // namespace world
