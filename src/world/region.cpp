#include "world/region.hpp"

#include <fcntl.h>
#include <libdeflate.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace world {

namespace {

constexpr u8 kMagic[8] = {'M', 'C', 'R', 'G', 'N', 0, 0, 1};
constexpr u32 kHeaderBytes = 16 + kRegionChunks * sizeof(RegionEntry);
constexpr u32 kHeaderSectors = (kHeaderBytes + kRegionSector - 1) / kRegionSector;
// Chosen without a measurement: the fast end of deflate. A chunk is written far less often than
// it is read, and inflating costs the same whatever made the stream.
constexpr int kLevel = 3;

libdeflate_compressor* compressor() {
    thread_local struct Holder {
        libdeflate_compressor* c = libdeflate_alloc_compressor(kLevel);
        ~Holder() { libdeflate_free_compressor(c); }
    } h;
    return h.c;
}
libdeflate_decompressor* decompressor() {
    thread_local struct Holder {
        libdeflate_decompressor* d = libdeflate_alloc_decompressor();
        ~Holder() { libdeflate_free_decompressor(d); }
    } h;
    return h.d;
}

std::string os_error(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

bool pread_all(int fd, u8* to, usize n, u64 at) {
    while (n) {
        const ssize_t got = ::pread(fd, to, n, static_cast<off_t>(at));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        to += got, at += static_cast<u64>(got), n -= static_cast<usize>(got);
    }
    return true;
}
bool pwrite_all(int fd, const u8* from, usize n, u64 at) {
    while (n) {
        const ssize_t put = ::pwrite(fd, from, n, static_cast<off_t>(at));
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        from += put, at += static_cast<u64>(put), n -= static_cast<usize>(put);
    }
    return true;
}

u32 sectors_for(u32 bytes) { return (bytes + kRegionSector - 1) / kRegionSector; }

void mark(std::vector<u64>& used, u32 first, u32 n, bool on) {
    for (u32 s = first; s < first + n; ++s) {
        if (on)
            used[s >> 6] |= 1ull << (s & 63);
        else
            used[s >> 6] &= ~(1ull << (s & 63));
    }
}
bool any_used(const std::vector<u64>& used, u32 first, u32 n) {
    for (u32 s = first; s < first + n; ++s)
        if ((used[s >> 6] >> (s & 63)) & 1) return true;
    return false;
}

template <class Inflate>
bool inflate_growing(std::span<const u8> stored, std::vector<u8>& out, Inflate&& inflate) {
    usize cap = std::max<usize>(stored.size() * 8, 64 * 1024);
    for (;;) {
        out.resize(cap);
        usize got = 0;
        const libdeflate_result r = inflate(decompressor(), stored.data(), stored.size(), out.data(), cap, &got);
        if (r == LIBDEFLATE_SUCCESS) {
            out.resize(got);
            return true;
        }
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE || cap > (1u << 30)) return false;
        cap *= 4;
    }
}

}  // namespace

void deflate_bytes(std::span<const u8> raw, std::vector<u8>& out) {
    out.resize(libdeflate_deflate_compress_bound(compressor(), raw.size()));
    out.resize(libdeflate_deflate_compress(compressor(), raw.data(), raw.size(), out.data(), out.size()));
}

bool inflate_bytes(std::span<const u8> stored, u8* raw, usize raw_bytes) {
    usize got = 0;
    return libdeflate_deflate_decompress(decompressor(), stored.data(), stored.size(), raw, raw_bytes, &got) ==
               LIBDEFLATE_SUCCESS &&
           got == raw_bytes;
}

bool inflate_zlib(std::span<const u8> stored, std::vector<u8>& out) {
    return inflate_growing(stored, out, libdeflate_zlib_decompress);
}
bool inflate_gzip(std::span<const u8> stored, std::vector<u8>& out) {
    return inflate_growing(stored, out, libdeflate_gzip_decompress);
}

u32 crc32_of(std::span<const u8> bytes) { return libdeflate_crc32(0, bytes.data(), bytes.size()); }

RegionFile::~RegionFile() { close(); }

void RegionFile::close() {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

bool RegionFile::open(const std::string& path, bool create, std::string& err) {
    err.clear();
    fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0644);
    if (fd < 0) {
        if (errno != ENOENT || create) err = os_error(path.c_str());
        return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        err = os_error(path.c_str());
        return false;
    }
    u8 head[16];
    if (st.st_size == 0) {  // a new region: nothing but a header
        std::memcpy(head, kMagic, 8);
        store_le<u32>(head + 8, kRegionSector);
        store_le<u32>(head + 12, 0);
        std::vector<u8> zero(kHeaderSectors * kRegionSector, 0);
        std::memcpy(zero.data(), head, sizeof head);
        if (!pwrite_all(fd, zero.data(), zero.size(), 0)) {
            err = os_error(path.c_str());
            return false;
        }
        st.st_size = static_cast<off_t>(zero.size());
    } else if (static_cast<u64>(st.st_size) < kHeaderBytes || !pread_all(fd, head, sizeof head, 0) ||
               std::memcmp(head, kMagic, 8) != 0 || load_le<u32>(head + 8) != kRegionSector) {
        err = path + ": not a region file of this server";
        return false;
    }
    if (!pread_all(fd, reinterpret_cast<u8*>(table), sizeof table, 16)) {
        err = os_error(path.c_str());
        return false;
    }
    sectors = std::max(kHeaderSectors, sectors_for(static_cast<u32>(std::min<u64>(static_cast<u64>(st.st_size), 0xFFFFFE00u))));
    used.assign((sectors + 63) / 64 + 1, 0);
    mark(used, 0, kHeaderSectors, true);
    for (u32 i = 0; i < kRegionChunks; ++i) {
        RegionEntry& e = table[i];
        if (e.sector == 0) continue;
        const u32 n = sectors_for(e.bytes);
        // An entry that points outside the file or into another chunk cannot be trusted with
        // either: it is treated as no chunk (its sectors, if they are its own, stay unused).
        if (e.sector < kHeaderSectors || e.bytes == 0 || static_cast<u64>(e.sector) + n > sectors ||
            any_used(used, e.sector, n)) {
            e = {};
            continue;
        }
        mark(used, e.sector, n, true);
    }
    return true;
}

bool RegionFile::write(std::span<const Put> batch, bool sync, std::string& err) {
    struct Placed {
        u32 index;
        RegionEntry now;
    };
    std::vector<Placed> placed;
    placed.reserve(batch.size());
    std::vector<u8> stored;
    bool ok = true;
    u32 hint = kHeaderSectors;
    for (const Put& put : batch) {
        deflate_bytes(put.raw, stored);
        const u32 n = sectors_for(static_cast<u32>(stored.size()));
        // First fit. The run may start past what the bitmap covers: that is the end of the file.
        u32 at = hint, run = 0;
        for (u32 s = hint; run < n; ++s) {
            if (s < sectors && ((used[s >> 6] >> (s & 63)) & 1)) {
                run = 0;
                at = s + 1;
            } else {
                ++run;
            }
        }
        if (at + n > sectors) {
            sectors = at + n;
            used.resize((sectors + 63) / 64 + 1, 0);
        }
        if (at == hint) hint = at + n;  // everything before it is taken: the next search starts behind it
        if (!pwrite_all(fd, stored.data(), stored.size(), static_cast<u64>(at) * kRegionSector)) {
            err = os_error("writing a chunk");
            ok = false;
            break;
        }
        mark(used, at, n, true);
        placed.push_back({put.index, {at, static_cast<u32>(stored.size()), static_cast<u32>(put.raw.size()), crc32_of(stored)}});
    }
    if (sync && ::fdatasync(fd) != 0) {
        err = os_error("syncing a region");
        ok = false;
    }
    if (!ok) {  // nothing points at what was written: its sectors are free again
        for (const Placed& p : placed) mark(used, p.now.sector, sectors_for(p.now.bytes), false);
        return false;
    }
    // The entries. A chunk that is in the batch twice keeps its last copy.
    std::vector<RegionEntry> old;
    old.reserve(placed.size());
    for (const Placed& p : placed) {
        old.push_back(table[p.index]);
        table[p.index] = p.now;
    }
    if (!pwrite_all(fd, reinterpret_cast<const u8*>(table), sizeof table, 16) || (sync && ::fdatasync(fd) != 0)) {
        err = os_error("writing a region's header");
        // Which entries reached the disk is not known: neither the old nor the new sectors are given up.
        return false;
    }
    for (const RegionEntry& e : old)
        if (e.sector) mark(used, e.sector, sectors_for(e.bytes), false);
    return true;
}

bool region_read(int fd, const RegionEntry& e, std::vector<u8>& raw, std::string& err) {
    std::vector<u8> stored(e.bytes);
    if (!pread_all(fd, stored.data(), stored.size(), static_cast<u64>(e.sector) * kRegionSector)) {
        err = "short read";
        return false;
    }
    if (crc32_of(stored) != e.crc) {
        err = "checksum does not match";
        return false;
    }
    raw.resize(e.raw_bytes);
    if (!inflate_bytes(stored, raw.data(), raw.size())) {
        err = "does not inflate";
        return false;
    }
    return true;
}

}  // namespace world
