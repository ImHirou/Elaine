// ChunkMap: (chunk x, chunk z) -> Chunk*, the lookup under every block access.
//
// Two levels: a region is a dense 32x32 grid of chunk pointers (the same granularity as an Anvil
// region file), and regions are found through a small open-addressing table with a direct-mapped
// cache in front of it. Why not one hash of chunks: almost every access pattern here is 2D-local
// (light crossing a chunk border, an entity's box, a player's view square), and in a grid the
// neighbouring chunks are the neighbouring pointers, in the same cache lines, with no hashing.
// Measured against std::unordered_map and a flat chunk hash in bench "chunk map".
//
// Not thread-safe: owned by the thread that owns the world.
#pragma once

#include <vector>

#include "world/chunk.hpp"

namespace world {

class ChunkMap {
  public:
    static constexpr u32 kRegionShift = 5;
    static constexpr u32 kRegionSize = 1u << kRegionShift;  // chunks per side

    ChunkMap() {
        table_.resize(64);
        for (Slot& c : cache_) c.key = kNoKey;
    }
    ChunkMap(const ChunkMap&) = delete;
    ChunkMap& operator=(const ChunkMap&) = delete;
    ~ChunkMap() {
        for (Slot& s : table_) delete s.region;
    }

    ALWAYS_INLINE Chunk* find(i32 cx, i32 cz) const {
        const Region* r = region(cx >> kRegionShift, cz >> kRegionShift);
        return LIKELY(r != nullptr) ? r->chunks[local(cx, cz)] : nullptr;
    }

    // The slot for (cx, cz), creating its region if needed. Null on entry means "no chunk here".
    Chunk*& slot(i32 cx, i32 cz) {
        const i32 rx = cx >> kRegionShift, rz = cz >> kRegionShift;
        Region* r = const_cast<Region*>(region(rx, rz));
        if (!r) r = add_region(rx, rz);
        return r->chunks[local(cx, cz)];
    }
    void insert(Chunk* c) {
        Chunk*& s = slot(c->x, c->z);
        DCHECK(s == nullptr);
        s = c;
        ++size_;
    }
    Chunk* remove(i32 cx, i32 cz) {
        Region* r = const_cast<Region*>(region(cx >> kRegionShift, cz >> kRegionShift));
        if (!r) return nullptr;
        Chunk* c = r->chunks[local(cx, cz)];
        if (c) {
            r->chunks[local(cx, cz)] = nullptr;
            --size_;
        }
        return c;
    }

    usize size() const { return size_; }
    usize regions() const { return regions_; }

    // Chunks that are being loaded: one bit per chunk beside its pointer. A chunk is asked for
    // once; the bit says that it has been.
    bool loading(i32 cx, i32 cz) const {
        const Region* r = region(cx >> kRegionShift, cz >> kRegionShift);
        return r && ((r->loading[local(cx, cz) >> 6] >> (local(cx, cz) & 63)) & 1);
    }
    void set_loading(i32 cx, i32 cz, bool on) {
        const i32 rx = cx >> kRegionShift, rz = cz >> kRegionShift;
        Region* r = const_cast<Region*>(region(rx, rz));
        if (!r) r = add_region(rx, rz);
        u64& w = r->loading[local(cx, cz) >> 6];
        const u64 bit = 1ull << (local(cx, cz) & 63);
        loading_ += on && !(w & bit);
        loading_ -= !on && (w & bit);
        w = on ? w | bit : w & ~bit;
    }
    usize loading_count() const { return loading_; }

    // Visit up to `budget` chunks, going on from where the last call stopped and around again:
    // with nothing coming or going every chunk is seen once in size() visits. `f` must not add or
    // remove chunks (collect, then act).
    template <class F>
    void scan(u32 budget, F&& f) {
        const usize slots = table_.size() * (kRegionSize * kRegionSize);
        for (usize walked = 0; budget && size_ && walked < slots;) {
            if (scan_ >= slots) scan_ = 0;
            const Slot& s = table_[scan_ / (kRegionSize * kRegionSize)];
            if (!s.region) {
                const usize next = (scan_ / (kRegionSize * kRegionSize) + 1) * (kRegionSize * kRegionSize);
                walked += next - scan_;
                scan_ = next;
                continue;
            }
            Chunk* c = s.region->chunks[scan_ % (kRegionSize * kRegionSize)];
            ++scan_;
            ++walked;
            if (c) {
                --budget;
                f(*c);
            }
        }
    }

    template <class F>
    void for_each(F&& f) const {
        for (const Slot& s : table_) {
            if (!s.region) continue;
            for (Chunk* c : s.region->chunks)
                if (c) f(*c);
        }
    }

  private:
    struct Region {
        Chunk* chunks[kRegionSize * kRegionSize] = {};
        u64 loading[kRegionSize * kRegionSize / 64] = {};
    };
    struct Slot {
        u64 key = 0;
        Region* region = nullptr;
    };
    // Direct-mapped front of the region table, indexed by the low bits of the region coordinates:
    // one compare for any region within a 16x16-region (8192 x 8192 block) window of the last one
    // that used the entry. Measured: scattered lookups 181 -> see bench "chunk map" (4x4 entries
    // lost to a flat hash of chunks there; 16x16 does not).
    static constexpr u32 kCacheBits = 4;
    static constexpr u32 kCacheSize = 1u << (2 * kCacheBits);
    // Marks an unused cache entry. Region x = -2^31 does not exist (region = chunk >> 5, so |x| < 2^26);
    // note that ~0 would NOT do: it is the key of region (-1, -1).
    static constexpr u64 kNoKey = 0x8000000000000000ull;

    ALWAYS_INLINE static u32 local(i32 cx, i32 cz) {
        return ((static_cast<u32>(cz) & (kRegionSize - 1)) << kRegionShift) | (static_cast<u32>(cx) & (kRegionSize - 1));
    }
    ALWAYS_INLINE static u64 key_of(i32 rx, i32 rz) {
        return (static_cast<u64>(static_cast<u32>(rx)) << 32) | static_cast<u32>(rz);
    }
    ALWAYS_INLINE static u32 hash(u64 key) { return static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> 32); }

    ALWAYS_INLINE const Region* region(i32 rx, i32 rz) const {
        const u64 key = key_of(rx, rz);
        // A region and its neighbours never share an entry, so walking back and forth across a
        // region border does not evict.
        const u32 m = (1u << kCacheBits) - 1;
        Slot& c = cache_[(static_cast<u32>(rx) & m) | ((static_cast<u32>(rz) & m) << kCacheBits)];
        if (LIKELY(c.key == key)) return c.region;
        return lookup(key, c);
    }
    NOINLINE const Region* lookup(u64 key, Slot& cache) const {
        const u32 mask = static_cast<u32>(table_.size()) - 1;
        for (u32 i = hash(key) & mask;; i = (i + 1) & mask) {
            const Slot& s = table_[i];
            if (!s.region) return nullptr;
            if (s.key == key) {
                cache = s;
                return s.region;
            }
        }
    }
    Region* add_region(i32 rx, i32 rz) {
        if ((regions_ + 1) * 2 > table_.size()) rehash(table_.size() * 2);
        Region* r = new Region();
        place(table_, key_of(rx, rz), r);
        ++regions_;
        return r;
    }
    static void place(std::vector<Slot>& t, u64 key, Region* r) {
        const u32 mask = static_cast<u32>(t.size()) - 1;
        u32 i = hash(key) & mask;
        while (t[i].region) i = (i + 1) & mask;
        t[i] = {key, r};
    }
    void rehash(usize n) {
        std::vector<Slot> t(n);
        for (const Slot& s : table_)
            if (s.region) place(t, s.key, s.region);
        table_.swap(t);
    }

    std::vector<Slot> table_;  // power-of-two size, at most half full; regions are never removed
    mutable Slot cache_[kCacheSize];
    usize size_ = 0;
    usize regions_ = 0;
    usize loading_ = 0;
    usize scan_ = 0;
};

}  // namespace world
