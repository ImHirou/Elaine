// ByteBuf: growable byte buffer used for every outbound stream and for frame reassembly.
//
// Encoders follow one pattern:   u8* p = buf.reserve(upper_bound);  p = put_x(p, ...);  buf.commit(p);
// i.e. one capacity check per packet, then unchecked pointer writes. kSlack bytes past the
// reserved region are always writable so encoders may use wide (8/16/32 byte) stores.
#pragma once

#include <cstdlib>

#include "base/types.hpp"

namespace base {

struct ByteBuf {
    static constexpr u32 kSlack = 64;

    u8* data = nullptr;
    u32 len = 0;              // bytes written
    u32 cap = 0;              // usable capacity, excluding slack
    u32 rd = 0;               // consumer offset (send progress / parse position)
    u32 tag = 0;              // owner-defined (the reactor stores which pool to return it to)
    ByteBuf* next = nullptr;  // intrusive link: send queues and free lists

    ByteBuf() = default;
    explicit ByteBuf(u32 initial) { grow(initial); }
    ByteBuf(const ByteBuf&) = delete;
    ByteBuf& operator=(const ByteBuf&) = delete;
    ByteBuf(ByteBuf&& o) noexcept : data(o.data), len(o.len), cap(o.cap), rd(o.rd), tag(o.tag), next(o.next) {
        o.data = nullptr;
        o.len = o.cap = o.rd = 0;
        o.next = nullptr;
    }
    ~ByteBuf() { std::free(data); }

    ALWAYS_INLINE u8* reserve(usize n) {
        if (UNLIKELY(len + n > cap)) grow(len + n);
        return data + len;
    }
    ALWAYS_INLINE void commit(const u8* end) {
        DCHECK(end >= data && static_cast<usize>(end - data) <= cap + kSlack);
        len = static_cast<u32>(end - data);
    }
    ALWAYS_INLINE void append(const void* src, usize n) {
        u8* p = reserve(n);
        std::memcpy(p, src, n);
        len += static_cast<u32>(n);
    }
    ALWAYS_INLINE void clear() { len = rd = 0; }
    void reset() {  // give the memory back (used when a buffer grew far beyond its steady state)
        std::free(data);
        data = nullptr;
        len = cap = rd = 0;
    }
    ALWAYS_INLINE u32 readable() const { return len - rd; }
    ALWAYS_INLINE bool empty() const { return len == rd; }

    // Drop already-consumed bytes so the buffer does not grow without bound.
    void compact() {
        if (rd == 0) return;
        if (rd != len) std::memmove(data, data + rd, len - rd);
        len -= rd;
        rd = 0;
    }

    NOINLINE void grow(usize need) {
        usize ncap = cap ? cap : 256;
        while (ncap < need) ncap *= 2;
        // 64-byte alignment: buffers are handed to SIMD kernels (cipher, compressor).
        usize bytes = (ncap + kSlack + 63) & ~usize{63};
        u8* nd = static_cast<u8*>(std::aligned_alloc(64, bytes));
        if (UNLIKELY(!nd)) std::abort();
        if (len) std::memcpy(nd, data, len);
        std::free(data);
        data = nd;
        cap = static_cast<u32>(ncap);
    }
};

// Free list of ByteBufs. NOT thread-safe: each thread that produces buffers owns one pool and
// gets its buffers back through an SPSC ring (see net::Reactor), never through shared state.
struct BufPool {
    ByteBuf* head = nullptr;
    u32 count = 0;
    u32 keep_cap = 1u << 20;  // buffers that grew beyond this are freed instead of pooled

    ByteBuf* get(u32 initial = 4096) {
        if (ByteBuf* b = head) {
            head = b->next;
            --count;
            b->next = nullptr;
            b->clear();
            return b;
        }
        return new ByteBuf(initial);
    }
    void put(ByteBuf* b) {
        if (b->cap > keep_cap || count >= 4096) {
            delete b;
            return;
        }
        b->next = head;
        head = b;
        ++count;
    }
    ~BufPool() {
        while (ByteBuf* b = head) {
            head = b->next;
            delete b;
        }
    }
};

}  // namespace base
