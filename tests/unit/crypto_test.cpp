// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <random>
#include <vector>

#include "crypto/auth.hpp"
#include "crypto/cfb8.hpp"

using namespace crypto;

namespace {

// Reference: OpenSSL's AES-128-CFB8 with key == iv.
struct Ref {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    Ref(const u8 key[16], bool encrypt) { EVP_CipherInit_ex(ctx, EVP_aes_128_cfb8(), nullptr, key, key, encrypt ? 1 : 0); }
    ~Ref() { EVP_CIPHER_CTX_free(ctx); }
    void run(u8* data, usize n) {
        std::vector<u8> out(n + 32);
        int len = 0;
        EVP_CipherUpdate(ctx, out.data(), &len, data, static_cast<int>(n));
        ASSERT_EQ(static_cast<usize>(len), n);
        std::memcpy(data, out.data(), n);
    }
};

std::vector<u8> random_vec(std::mt19937& rng, usize n) {
    std::vector<u8> v(n);
    for (auto& b : v) b = static_cast<u8>(rng());
    return v;
}

}  // namespace

TEST(Cfb8, EncryptMatchesOpenSslAcrossChunkings) {
    std::mt19937 rng(42);
    const auto key = random_vec(rng, 16);
    const auto plain = random_vec(rng, 10000);

    auto expect = plain;
    Ref ref(key.data(), true);
    ref.run(expect.data(), expect.size());

    for (usize chunk : {1u, 3u, 15u, 16u, 17u, 64u, 1000u, 10000u}) {
        Cfb8 c;
        c.init(key.data());
        auto got = plain;
        for (usize off = 0; off < got.size(); off += chunk) c.encrypt(got.data() + off, std::min(chunk, got.size() - off));
        EXPECT_EQ(got, expect) << "chunk " << chunk;
    }
}

TEST(Cfb8, DecryptMatchesOpenSslAcrossChunkings) {
    std::mt19937 rng(43);
    const auto key = random_vec(rng, 16);
    const auto plain = random_vec(rng, 10000);
    auto cipher = plain;
    Ref ref(key.data(), true);
    ref.run(cipher.data(), cipher.size());

    for (usize chunk : {1u, 2u, 7u, 15u, 16u, 17u, 23u, 24u, 25u, 31u, 32u, 33u, 100u, 2048u, 10000u}) {
        Cfb8 c;
        c.init(key.data());
        auto got = cipher;
        for (usize off = 0; off < got.size(); off += chunk) c.decrypt(got.data() + off, std::min(chunk, got.size() - off));
        EXPECT_EQ(got, plain) << "chunk " << chunk;
    }
}

TEST(Cfb8, MixedChunkSizesDecrypt) {
    std::mt19937 rng(44);
    const auto key = random_vec(rng, 16);
    const auto plain = random_vec(rng, 50000);
    auto data = plain;
    Cfb8 enc, dec;
    enc.init(key.data());
    dec.init(key.data());
    enc.encrypt(data.data(), data.size());
    for (usize off = 0; off < data.size();) {
        const usize n = std::min<usize>(1 + rng() % 70, data.size() - off);
        dec.decrypt(data.data() + off, n);
        off += n;
    }
    EXPECT_EQ(data, plain);
}

TEST(Cfb8, BatchEqualsSerial) {
    std::mt19937 rng(45);
    for (int round = 0; round < 50; ++round) {
        const usize njobs = rng() % 40;
        std::vector<std::vector<u8>> bufs, expect;
        std::vector<Cfb8> batch_states(njobs), serial_states(njobs);
        std::vector<Cfb8Job> jobs;
        for (usize i = 0; i < njobs; ++i) {
            const auto key = random_vec(rng, 16);
            batch_states[i].init(key.data());
            serial_states[i].init(key.data());
            const usize len = (rng() % 8 == 0) ? 0 : rng() % 3000;
            bufs.push_back(random_vec(rng, len));
            expect.push_back(bufs.back());
            serial_states[i].encrypt(expect.back().data(), len);
        }
        for (usize i = 0; i < njobs; ++i) jobs.push_back({&batch_states[i], bufs[i].data(), bufs[i].size()});
        cfb8_encrypt_batch(jobs);
        for (usize i = 0; i < njobs; ++i) {
            EXPECT_EQ(bufs[i], expect[i]) << "round " << round << " job " << i;
            // The register must be left where the serial cipher leaves it, so streams can continue.
            EXPECT_EQ(0, std::memcmp(&batch_states[i].sr, &serial_states[i].sr, 16)) << "round " << round << " job " << i;
        }
    }
}

TEST(Auth, ServerHashKnownVectors) {
    // The three canonical examples of Minecraft's signed SHA-1 hex digest.
    auto h = [](std::string_view s) { return server_hash(s, {}, {}); };
    EXPECT_EQ(h("Notch"), "4ed1f46bbe04bc756bcb17c0c7ce3e4632f06a48");
    EXPECT_EQ(h("jeb_"), "-7c9d5b0044c130109a5d7b5fb5c317c02b4e28c1");
    EXPECT_EQ(h("simon"), "88e16a1019277b15d58faf0541e11910eb756f6");
}

TEST(Auth, OfflineUuid) { EXPECT_EQ(uuid_to_string(offline_uuid("Notch")), "b50ad385-829d-3141-a216-7e7d7539ba7f"); }

TEST(Auth, UuidStrings) {
    Uuid u;
    ASSERT_TRUE(uuid_from_string("069a79f444e94726a5befca90e38aaf5", u));
    EXPECT_EQ(uuid_to_string(u), "069a79f4-44e9-4726-a5be-fca90e38aaf5");
    Uuid v;
    ASSERT_TRUE(uuid_from_string("069a79f4-44e9-4726-a5be-fca90e38aaf5", v));
    EXPECT_EQ(u, v);
    EXPECT_FALSE(uuid_from_string("069a79f4", v));
    EXPECT_FALSE(uuid_from_string("z69a79f444e94726a5befca90e38aaf5", v));
}

TEST(Auth, RsaRoundTrip) {
    ServerKey key;
    EVP_PKEY* pub = nullptr;
    const u8* der = key.public_der().data();
    pub = d2i_PUBKEY(nullptr, &der, static_cast<long>(key.public_der().size()));
    ASSERT_NE(pub, nullptr);
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pub, nullptr);
    ASSERT_GT(EVP_PKEY_encrypt_init(ctx), 0);
    const u8 secret[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    u8 enc[256];
    size_t enc_len = sizeof enc;
    ASSERT_GT(EVP_PKEY_encrypt(ctx, enc, &enc_len, secret, sizeof secret), 0);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pub);

    u8 out[256];
    ASSERT_EQ(key.decrypt({enc, enc_len}, out, sizeof out), 16);
    EXPECT_EQ(0, std::memcmp(out, secret, 16));
}
