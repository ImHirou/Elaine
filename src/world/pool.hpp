// BlockPool: fixed-size, cache-line-aligned blocks carved from large slabs.
//
// The world allocates a handful of sizes over and over (one per palette width, light arrays): a
// free list per size is O(1), has no per-block header, cannot fragment across sizes, and gives the
// 64-byte alignment the SIMD kernels want for free. NOT thread-safe - a pool belongs to the thread
// that owns the world data (today: the tick thread).
//
// Under ASan freed blocks are poisoned, so a stale Section or light pointer still faults.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "base/types.hpp"

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define WORLD_POISON(p, n) ASAN_POISON_MEMORY_REGION(p, n)
#define WORLD_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
#else
#define WORLD_POISON(p, n) ((void)0)
#define WORLD_UNPOISON(p, n) ((void)0)
#endif

namespace world {

class BlockPool {
  public:
    BlockPool() = default;
    explicit BlockPool(u32 block_bytes) { init(block_bytes); }
    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;
    ~BlockPool() {
        for (void* s : slabs_) {
            WORLD_UNPOISON(s, static_cast<usize>(size_) * per_slab_);
            std::free(s);
        }
    }

    void init(u32 block_bytes) {
        DCHECK(slabs_.empty() && block_bytes >= sizeof(Node));
        size_ = (block_bytes + 63u) & ~63u;
        per_slab_ = std::max<u32>(8, kSlabBytes / size_);
    }

    ALWAYS_INLINE void* alloc() {
        if (UNLIKELY(!free_)) refill();
        Node* n = free_;
        WORLD_UNPOISON(n, size_);
        free_ = n->next;
        ++live_;
        return n;
    }
    ALWAYS_INLINE void free(void* p) {
        Node* n = static_cast<Node*>(p);
        n->next = free_;
        WORLD_POISON(n + 1, size_ - sizeof(Node));
        free_ = n;
        --live_;
    }

    u32 block_bytes() const { return size_; }
    usize live() const { return live_; }
    usize reserved_bytes() const { return slabs_.size() * static_cast<usize>(size_) * per_slab_; }

  private:
    static constexpr u32 kSlabBytes = 256 * 1024;
    struct Node {
        Node* next;
    };

    NOINLINE void refill() {
        u8* slab = static_cast<u8*>(std::aligned_alloc(64, static_cast<usize>(size_) * per_slab_));
        if (UNLIKELY(!slab)) std::abort();
        slabs_.push_back(slab);
        // Thread the blocks in address order so consecutive allocations are neighbours in memory.
        for (u32 i = per_slab_; i-- > 0;) {
            Node* n = reinterpret_cast<Node*>(slab + static_cast<usize>(i) * size_);
            n->next = free_;
            WORLD_POISON(n + 1, size_ - sizeof(Node));
            free_ = n;
        }
    }

    Node* free_ = nullptr;
    std::vector<void*> slabs_;
    u32 size_ = 0;
    u32 per_slab_ = 0;
    usize live_ = 0;
};

}  // namespace world
