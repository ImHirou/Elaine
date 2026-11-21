// SPDX-License-Identifier: GPL-3.0-or-later
// Single-producer / single-consumer queues: the only way threads exchange data on hot paths.
//
//   SpscRing<T>   fixed-size trivially-copyable items (handles, pointers, small messages)
//   SpscBytes     variable-size records in one contiguous arena (packets net -> tick)
//
// Both keep producer and consumer indices on separate cache lines and cache the other side's
// index, so an uncontended push/pop touches no shared line. Producers may batch: records become
// visible to the consumer only at publish().
#pragma once

#include <atomic>
#include <cstdlib>
#include <type_traits>

#include "base/types.hpp"

namespace base {

template <class T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>);

  public:
    explicit SpscRing(u32 capacity_pow2) : mask_(capacity_pow2 - 1) {
        DCHECK(std::has_single_bit(capacity_pow2));
        slots_ = static_cast<T*>(std::aligned_alloc(64, ((sizeof(T) * capacity_pow2) + 63) & ~usize{63}));
        if (!slots_) std::abort();
    }
    ~SpscRing() { std::free(slots_); }
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer. Returns false when full (caller decides: retry, drop, or disconnect).
    ALWAYS_INLINE bool push(const T& v) {
        const u64 t = tail_.load(std::memory_order_relaxed);
        if (UNLIKELY(t - head_cache_ > mask_)) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ > mask_) return false;
        }
        slots_[t & mask_] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer.
    ALWAYS_INLINE bool pop(T& out) {
        const u64 h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = slots_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

  private:
    T* slots_;
    const u64 mask_;
    CACHE_ALIGNED std::atomic<u64> tail_{0};
    u64 head_cache_ = 0;
    CACHE_ALIGNED std::atomic<u64> head_{0};
    u64 tail_cache_ = 0;
};

class SpscBytes {
  public:
    explicit SpscBytes(u32 capacity_pow2) : cap_(capacity_pow2), mask_(capacity_pow2 - 1) {
        DCHECK(std::has_single_bit(capacity_pow2) && capacity_pow2 >= 64);
        mem_ = static_cast<u8*>(std::aligned_alloc(64, capacity_pow2));
        if (!mem_) std::abort();
    }
    ~SpscBytes() { std::free(mem_); }
    SpscBytes(const SpscBytes&) = delete;
    SpscBytes& operator=(const SpscBytes&) = delete;

    // Producer: reserve a record of n payload bytes; write into the returned pointer, then
    // commit(). Records never wrap (a pad record fills the arena tail). Null when full.
    u8* alloc(u32 n) {
        const u32 need = kHdr + ((n + 7u) & ~7u);
        DCHECK(need <= cap_ / 2);
        u32 pos = static_cast<u32>(tail_local_ & mask_);
        u32 pad = (pos + need > cap_) ? cap_ - pos : 0;
        if (UNLIKELY(tail_local_ + pad + need - head_cache_ > cap_)) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail_local_ + pad + need - head_cache_ > cap_) return nullptr;
        }
        if (UNLIKELY(pad)) {
            store_le<u32>(mem_ + pos, kPadMark);
            tail_local_ += pad;
            pos = 0;
        }
        store_le<u32>(mem_ + pos, n);
        pending_ = need;
        return mem_ + pos + kHdr;
    }
    ALWAYS_INLINE void commit() { tail_local_ += pending_; }
    // Make everything committed so far visible to the consumer (one shared-line write per batch).
    ALWAYS_INLINE void publish() { tail_.store(tail_local_, std::memory_order_release); }

    // Consumer: view the next record; valid until pop().
    bool front(const u8*& payload, u32& n) {
        for (;;) {
            if (head_local_ == tail_cache_) {
                tail_cache_ = tail_.load(std::memory_order_acquire);
                if (head_local_ == tail_cache_) return false;
            }
            const u32 pos = static_cast<u32>(head_local_ & mask_);
            const u32 size = load_le<u32>(mem_ + pos);
            if (UNLIKELY(size == kPadMark)) {
                head_local_ += cap_ - pos;
                continue;
            }
            payload = mem_ + pos + kHdr;
            n = size;
            return true;
        }
    }
    ALWAYS_INLINE void pop(u32 n) { head_local_ += kHdr + ((n + 7u) & ~7u); }
    // Hand consumed space back to the producer (once per drain, not per record).
    ALWAYS_INLINE void release() { head_.store(head_local_, std::memory_order_release); }

  private:
    static constexpr u32 kHdr = 8;  // u32 size + 4 pad: payloads stay 8-byte aligned
    static constexpr u32 kPadMark = 0xFFFFFFFFu;

    u8* mem_;
    const u32 cap_;
    const u64 mask_;
    CACHE_ALIGNED std::atomic<u64> tail_{0};
    u64 tail_local_ = 0;
    u64 head_cache_ = 0;
    u32 pending_ = 0;
    CACHE_ALIGNED std::atomic<u64> head_{0};
    u64 head_local_ = 0;
    u64 tail_cache_ = 0;
};

}  // namespace base
