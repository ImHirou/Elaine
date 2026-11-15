// AES-128-CFB8, the protocol's stream cipher (key == IV == shared secret), on AES-NI.
//
// CFB8 costs one AES block per *byte*, and encryption is serial per stream (each byte's
// keystream depends on the previous ciphertext byte), so a single stream is latency-bound
// at ~40 cycles/byte. Two things get the throughput back:
//   * decrypt(): the feedback is ciphertext we already have, so 8 blocks run in parallel.
//   * encrypt_batch(): encrypts up to 8 independent streams (connections) interleaved, filling
//     the AES unit's pipeline. This is what the tick flush uses.
#pragma once

#include <immintrin.h>

#include <span>

#include "base/types.hpp"

namespace crypto {

struct Cfb8 {
    __m128i rk[11];  // AES-128 encryption round keys (CFB only ever uses the forward cipher)
    __m128i sr;      // shift register: the last 16 ciphertext bytes

    void init(const u8 key[16]);  // IV = key, as the protocol specifies
    void encrypt(u8* data, usize n);
    void decrypt(u8* data, usize n);
};

struct Cfb8Job {
    Cfb8* state;
    u8* data;
    usize n;
};

// In-place encryption of many independent streams. Equivalent to calling encrypt() per job.
// Every job must reference a distinct Cfb8 state (one job per connection per flush).
void cfb8_encrypt_batch(std::span<Cfb8Job> jobs);

}  // namespace crypto
