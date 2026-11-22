// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto/cfb8.hpp"

#include <immintrin.h>

namespace crypto {

namespace {

ALWAYS_INLINE __m128i expand_step(__m128i key, __m128i gen) {
    gen = _mm_shuffle_epi32(gen, _MM_SHUFFLE(3, 3, 3, 3));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    return _mm_xor_si128(key, gen);
}

ALWAYS_INLINE __m128i aes_block(const __m128i* rk, __m128i b) {
    b = _mm_xor_si128(b, rk[0]);
    for (int r = 1; r < 10; ++r) b = _mm_aesenc_si128(b, rk[r]);
    return _mm_aesenclast_si128(b, rk[10]);
}

// Shift the register left by one byte and append ciphertext byte c.
ALWAYS_INLINE __m128i feed(__m128i sr, u8 c) { return _mm_insert_epi8(_mm_srli_si128(sr, 1), c, 15); }

constexpr int kLanes = 8;

}  // namespace

void Cfb8::init(const u8 key[16]) {
    __m128i k = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));
    rk[0] = k;
#define EXPAND(i, rcon)                                     \
    k = expand_step(k, _mm_aeskeygenassist_si128(k, rcon)); \
    rk[i] = k
    EXPAND(1, 0x01);
    EXPAND(2, 0x02);
    EXPAND(3, 0x04);
    EXPAND(4, 0x08);
    EXPAND(5, 0x10);
    EXPAND(6, 0x20);
    EXPAND(7, 0x40);
    EXPAND(8, 0x80);
    EXPAND(9, 0x1B);
    EXPAND(10, 0x36);
#undef EXPAND
    sr = rk[0];
}

void Cfb8::encrypt(u8* data, usize n) {
    __m128i s = sr;
    for (usize i = 0; i < n; ++i) {
        const u8 c = data[i] ^ static_cast<u8>(_mm_cvtsi128_si32(aes_block(rk, s)));
        data[i] = c;
        s = feed(s, c);
    }
    sr = s;
}

void Cfb8::decrypt(u8* data, usize n) {
    if (n == 0) return;
    // The register for byte i is ciphertext[i-16 .. i-1] (IV-prefixed), all known up front.
    // Walk backwards so in-place plaintext never overwrites ciphertext still needed as feedback.
    alignas(16) u8 head[32];  // [old register][first 16 ciphertext bytes]
    _mm_store_si128(reinterpret_cast<__m128i*>(head), sr);
    const usize hn = n < 16 ? n : 16;
    std::memcpy(head + 16, data, hn);

    // New register = last 16 bytes of (old register || ciphertext); capture before overwriting.
    __m128i next;
    if (n >= 16)
        next = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + n - 16));
    else
        next = _mm_loadu_si128(reinterpret_cast<const __m128i*>(head + n));

    usize i = n;
    // Positions >= 16: the register lies entirely inside `data`. Fixed-width batches keep all
    // eight blocks in registers (a variable trip count would spill them every round).
    while (i >= 16 + kLanes) {
        const usize base = i - kLanes;
        __m128i b[kLanes];
        for (int k = 0; k < kLanes; ++k)
            b[k] = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + base + k - 16)), rk[0]);
        for (int r = 1; r < 10; ++r)
            for (int k = 0; k < kLanes; ++k) b[k] = _mm_aesenc_si128(b[k], rk[r]);
        u8 ks[kLanes];
        for (int k = 0; k < kLanes; ++k) ks[k] = static_cast<u8>(_mm_cvtsi128_si32(_mm_aesenclast_si128(b[k], rk[10])));
        store_le<u64>(data + base, load_le<u64>(data + base) ^ load_le<u64>(ks));
        i = base;
    }
    while (i > 16) {
        --i;
        const __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i - 16));
        data[i] ^= static_cast<u8>(_mm_cvtsi128_si32(aes_block(rk, s)));
    }
    for (usize k = 0; k < hn; ++k) {  // positions < 16: register straddles the old IV
        const __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(head + k));
        data[k] = head[16 + k] ^ static_cast<u8>(_mm_cvtsi128_si32(aes_block(rk, s)));
    }
    sr = next;
}

namespace {

struct Lane {
    Cfb8* st;
    u8* p;
    usize left;
};

// Advance W streams by `run` bytes each, with the 10 AES rounds of all W interleaved. W is a
// compile-time constant so states and blocks live in registers; the loop-carried dependency of
// one stream (AES latency) is hidden behind the other W-1.
template <int W>
ALWAYS_INLINE void run_lanes(Lane* lane, usize run) {
    __m128i s[W];
    const __m128i* k[W];
    u8* p[W];
    for (int l = 0; l < W; ++l) {
        s[l] = lane[l].st->sr;
        k[l] = lane[l].st->rk;
        p[l] = lane[l].p;
    }
    for (usize i = 0; i < run; ++i) {
        __m128i b[W];
        for (int l = 0; l < W; ++l) b[l] = _mm_xor_si128(s[l], k[l][0]);
        for (int r = 1; r < 10; ++r)
            for (int l = 0; l < W; ++l) b[l] = _mm_aesenc_si128(b[l], k[l][r]);
        for (int l = 0; l < W; ++l) {
            b[l] = _mm_aesenclast_si128(b[l], k[l][10]);
            const u8 c = p[l][i] ^ static_cast<u8>(_mm_cvtsi128_si32(b[l]));
            p[l][i] = c;
            s[l] = feed(s[l], c);
        }
    }
    for (int l = 0; l < W; ++l) {
        lane[l].st->sr = s[l];
        lane[l].p += run;
        lane[l].left -= run;
    }
}

}  // namespace

void cfb8_encrypt_batch(std::span<Cfb8Job> jobs) {
    // Lane scheduler: keep up to 8 streams in flight; when one finishes, the next job takes its
    // lane. Once fewer than 8 jobs remain the width steps down 4 -> 2 -> 1.
    Lane lane[kLanes];
    int active = 0;
    usize next_job = 0;
    auto refill = [&](int l) -> bool {
        while (next_job < jobs.size()) {
            Cfb8Job& j = jobs[next_job++];
            if (j.n == 0) continue;
            lane[l] = {j.state, j.data, j.n};
            return true;
        }
        return false;
    };
    while (active < kLanes && refill(active)) ++active;

    while (active > 0) {
        const int w = active >= 8 ? 8 : active >= 4 ? 4 : active >= 2 ? 2 : 1;
        usize run = lane[0].left;
        for (int l = 1; l < w; ++l) run = lane[l].left < run ? lane[l].left : run;
        switch (w) {
            case 8: run_lanes<8>(lane, run); break;
            case 4: run_lanes<4>(lane, run); break;
            case 2: run_lanes<2>(lane, run); break;
            default:
                lane[0].st->encrypt(lane[0].p, run);
                lane[0].left -= run;
                break;
        }
        for (int l = 0; l < w && l < active;) {
            if (lane[l].left == 0 && !refill(l)) {
                lane[l] = lane[--active];  // compact; the lane moved into slot l is examined next
                continue;
            }
            ++l;
        }
    }
}

}  // namespace crypto
