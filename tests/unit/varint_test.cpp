#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "proto/varint.hpp"

using namespace proto;

namespace {

std::vector<u8> reference_encode(u32 v) {
    std::vector<u8> out;
    do {
        u8 b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
    return out;
}

std::vector<u32> interesting_values() {
    std::vector<u32> v = {0,       1,       2,         127,       128,        129,         255,         256,   16383, 16384,
                          2097151, 2097152, 268435455, 268435456, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 25565, 777};
    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) v.push_back(rng() >> (rng() % 32));
    return v;
}

}  // namespace

TEST(VarInt, SizeMatchesReference) {
    for (u32 v : interesting_values()) EXPECT_EQ(varint_size(v), reference_encode(v).size()) << v;
}

TEST(VarInt, EncodeMatchesReference) {
    for (u32 v : interesting_values()) {
        u8 buf[16] = {};
        const u8* end = put_varint(buf, v);
        const auto ref = reference_encode(v);
        ASSERT_EQ(static_cast<usize>(end - buf), ref.size()) << v;
        EXPECT_EQ(0, std::memcmp(buf, ref.data(), ref.size())) << v;
    }
}

TEST(VarInt, DecodeFastAndSlowAgree) {
    for (u32 v : interesting_values()) {
        u8 buf[16];
        std::memset(buf, 0xFF, sizeof buf);  // garbage after the value must not matter
        const u32 n = static_cast<u32>(put_varint(buf, v) - buf);
        buf[n] = 0xFF;
        u32 fast = 0, slow = 0;
        EXPECT_EQ(get_varint_fast(buf, fast), n) << v;
        EXPECT_EQ(fast, v);
        EXPECT_EQ(get_varint_slow(buf, buf + n, slow), n) << v;
        EXPECT_EQ(slow, v);
        if (n > 1) {  // truncated input: slow path asks for more
            u32 dummy = 0;
            EXPECT_EQ(get_varint_slow(buf, buf + n - 1, dummy), 0u);
        }
    }
}

TEST(VarInt, PaddedThreeByteFormDecodes) {
    for (u32 v : {0u, 1u, 127u, 128u, 300u, 16384u, 2097151u}) {
        u8 buf[16] = {};
        put_varint3(buf, v);
        u32 out = 0;
        EXPECT_EQ(get_varint_fast(buf, out), 3u);
        EXPECT_EQ(out, v);
        EXPECT_EQ(get_varint_slow(buf, buf + 3, out), 3u);
        EXPECT_EQ(out, v);
    }
}

TEST(VarInt, RejectsOverlongInput) {
    u8 buf[16];
    std::memset(buf, 0x80, sizeof buf);
    u32 out = 0;
    EXPECT_EQ(get_varint_fast(buf, out), 0u);
    EXPECT_EQ(get_varint_slow(buf, buf + 16, out), ~0u);
    EXPECT_EQ(get_varint(buf, buf + 16, out), ~0u);
    EXPECT_EQ(get_varint(buf, buf + 6, out), ~0u);
}
