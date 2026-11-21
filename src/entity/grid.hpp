// SPDX-License-Identifier: GPL-3.0-or-later
// Grid: which entities stand in which square of the world. The spatial index behind "what could
// this player see from here" once there are more entities than it pays to test one by one.
//
// A cell is a 64 x 64 block column of the world, holding the dense indices of the entities in it.
// The size follows the question, not the chunk: a player sees entities up to its view distance
// (160 blocks at the default 10 chunks), so a query touches 6 x 6 cells; chunk-sized cells would
// make that 21 x 21 lookups for the same handful of entities, and section-sized ones (16^3) more
// still - measured in bench "tracker" (32 / 64 / 128 block cells: see docs/architecture.md).
// Small-radius queries want small cells of their own, which is why the cell size is a parameter:
// `Grid` (64 blocks) is the tracker's; the game keeps items in a BasicGrid<1> (2 blocks) for
// "what lies within reach of this player".
//
// Two levels, like world::ChunkMap and for the same reason: a region is a dense 8 x 8 array of
// cells (512 x 512 blocks), found through a small hash; a query box spans at most a few regions,
// and inside one, neighbouring cells are neighbouring array slots.
//
// Only entities that are not players are kept here: players are few, contiguous in the entity
// table, and move every tick - scanning them is cheaper than keeping them sorted.
//
// Tick thread only.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "base/types.hpp"

namespace entity {

template <u32 kShift>
class BasicGrid {
  public:
    static constexpr u32 kCellShift = kShift;  // log2(cell edge in blocks)
    static constexpr u32 kRegionShift = 3;     // log2(cells per region edge)
    static constexpr u32 kRegionCells = 1u << kRegionShift;

    struct Cell {
        u32* items = nullptr;  // dense entity indices, in no order
        u32 n = 0, cap = 0;
    };

    BasicGrid() { table_.resize(64); }
    BasicGrid(const BasicGrid&) = delete;
    BasicGrid& operator=(const BasicGrid&) = delete;
    ~BasicGrid() {
        for (Slot& s : table_) {
            if (!s.region) continue;
            for (Cell& c : s.region->cells) std::free(c.items);
            delete s.region;
        }
    }

    // The cell coordinate of a block coordinate.
    static constexpr i32 cell_of(i32 block) { return block >> kCellShift; }

    // Puts entity `e` into cell (cx, cz); returns its position in the cell, to be remembered.
    u32 add(i32 cx, i32 cz, u32 e) {
        Cell& c = cell(cx, cz);
        if (UNLIKELY(c.n == c.cap)) {
            c.cap = c.cap ? c.cap * 2 : 4;
            c.items = static_cast<u32*>(std::realloc(c.items, c.cap * sizeof(u32)));
            if (UNLIKELY(!c.items)) std::abort();
        }
        c.items[c.n] = e;
        ++size_;
        return c.n++;
    }
    // Takes out the entity at `pos` of cell (cx, cz). The cell's last entity fills the gap: its
    // index is returned so that its remembered position can be set to `pos` (~0u if none moved).
    u32 remove(i32 cx, i32 cz, u32 pos) {
        Cell& c = cell(cx, cz);
        DCHECK(pos < c.n);
        --size_;
        const u32 last = --c.n;
        if (pos == last) return ~0u;
        return c.items[pos] = c.items[last];
    }
    // The entity at `pos` of cell (cx, cz) has a new dense index.
    void rename(i32 cx, i32 cz, u32 pos, u32 e) {
        Cell& c = cell(cx, cz);
        DCHECK(pos < c.n);
        c.items[pos] = e;
    }

    // f(e) for every entity in the cells [cx0, cx1] x [cz0, cz1].
    template <class F>
    void for_each(i32 cx0, i32 cz0, i32 cx1, i32 cz1, F&& f) const {
        for (i32 rz = cz0 >> kRegionShift; rz <= cz1 >> kRegionShift; ++rz) {
            for (i32 rx = cx0 >> kRegionShift; rx <= cx1 >> kRegionShift; ++rx) {
                const Region* r = find(rx, rz);
                if (!r) continue;
                const i32 bx = rx << kRegionShift, bz = rz << kRegionShift;  // first cell of the region
                const i32 x0 = std::max(cx0 - bx, 0), x1 = std::min<i32>(cx1 - bx, kRegionCells - 1);
                const i32 z0 = std::max(cz0 - bz, 0), z1 = std::min<i32>(cz1 - bz, kRegionCells - 1);
                for (i32 z = z0; z <= z1; ++z) {
                    for (i32 x = x0; x <= x1; ++x) {
                        const Cell& c = r->cells[(z << kRegionShift) | x];
                        for (u32 i = 0; i < c.n; ++i) f(c.items[i]);
                    }
                }
            }
        }
    }

    usize size() const { return size_; }

  private:
    struct Region {
        Cell cells[kRegionCells * kRegionCells];
    };
    struct Slot {
        u64 key = 0;
        Region* region = nullptr;
    };

    static u64 key_of(i32 rx, i32 rz) { return (static_cast<u64>(static_cast<u32>(rx)) << 32) | static_cast<u32>(rz); }
    static u32 hash(u64 key) { return static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> 32); }

    const Region* find(i32 rx, i32 rz) const {
        const u64 key = key_of(rx, rz);
        if (LIKELY(last_.region && last_.key == key)) return last_.region;  // queries and moves are local
        const u32 mask = static_cast<u32>(table_.size()) - 1;
        for (u32 i = hash(key) & mask;; i = (i + 1) & mask) {
            const Slot& s = table_[i];
            if (!s.region) return nullptr;
            if (s.key == key) {
                last_ = s;
                return s.region;
            }
        }
    }
    Cell& cell(i32 cx, i32 cz) {
        const i32 rx = cx >> kRegionShift, rz = cz >> kRegionShift;
        Region* r = const_cast<Region*>(find(rx, rz));
        if (UNLIKELY(!r)) {
            if ((regions_ + 1) * 2 > table_.size()) rehash(table_.size() * 2);
            r = new Region();
            place(table_, key_of(rx, rz), r);
            ++regions_;
        }
        return r
            ->cells[((static_cast<u32>(cz) & (kRegionCells - 1)) << kRegionShift) | (static_cast<u32>(cx) & (kRegionCells - 1))];
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
    mutable Slot last_;        // the region asked for last
    usize regions_ = 0;
    usize size_ = 0;
};

// The tracker's grid: 64-block cells (see top).
using Grid = BasicGrid<6>;

}  // namespace entity
