// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <string>

#include "proto/frame.hpp"
#include "proto/wire.hpp"

using namespace proto;

namespace {
struct Blob {
    static constexpr u32 kId = 0x85;  // two-byte id on purpose
    std::string data;
    usize bound() const { return data.size(); }
    u8* encode(u8* p) const { return put_bytes(p, data.data(), data.size()); }
};

std::string body_of(const Blob& b) {
    std::string s = "\x85\x01";
    return s + b.data;
}
}  // namespace

TEST(Frame, UncompressedRoundTrip) {
    base::ByteBuf out;
    const Blob a{"hello"}, b{std::string(5000, 'x')};
    write_packet(out, kNoCompression, a);
    write_packet(out, kNoCompression, b);

    const u8* p = out.data;
    const u8* end = out.data + out.len;
    std::span<const u8> body;
    u32 used = 0;
    ASSERT_EQ(frame_split(p, end, kMaxFrame, body, used), FrameStatus::Ok);
    EXPECT_EQ(std::string(body.begin(), body.end()), body_of(a));
    p += used;
    ASSERT_EQ(frame_split(p, end, kMaxFrame, body, used), FrameStatus::Ok);
    EXPECT_EQ(std::string(body.begin(), body.end()), body_of(b));
    p += used;
    EXPECT_EQ(p, end);
}

TEST(Frame, CompressedRoundTrip) {
    std::mt19937 rng(7);
    base::ByteBuf out;
    std::vector<Blob> blobs;
    blobs.push_back({"tiny"});                    // below threshold: stored
    blobs.push_back({std::string(253, 'a')});     // body = 255 bytes: still stored
    blobs.push_back({std::string(254, 'a')});     // body = 256 bytes: compressed
    blobs.push_back({std::string(100000, 'z')});  // very compressible
    std::string noise(70000, 0);
    for (auto& ch : noise) ch = static_cast<char>(rng());
    blobs.push_back({noise});  // incompressible: grows a little
    for (const auto& b : blobs) write_packet(out, 256, b);

    const u8* p = out.data;
    const u8* end = out.data + out.len;
    for (usize i = 0; i < blobs.size(); ++i) {
        std::span<const u8> frame, body;
        u32 used = 0;
        ASSERT_EQ(frame_split(p, end, kMaxFrame, frame, used), FrameStatus::Ok) << i;
        const bool stored = frame[0] == 0;
        EXPECT_EQ(stored, body_of(blobs[i]).size() < 256) << i;
        ASSERT_TRUE(frame_inflate(frame, 256, body)) << i;
        EXPECT_EQ(std::string(body.begin(), body.end()), body_of(blobs[i])) << i;
        p += used;
    }
    EXPECT_EQ(p, end);
}

TEST(Frame, SplitNeedsMoreOnEveryPrefix) {
    base::ByteBuf out;
    write_packet(out, kNoCompression, Blob{std::string(300, 'q')});
    std::span<const u8> body;
    u32 used = 0;
    for (u32 n = 0; n < out.len; ++n)
        EXPECT_EQ(frame_split(out.data, out.data + n, kMaxFrame, body, used), FrameStatus::NeedMore) << n;
    EXPECT_EQ(frame_split(out.data, out.data + out.len, kMaxFrame, body, used), FrameStatus::Ok);
    EXPECT_EQ(used, out.len);
}

TEST(Frame, SplitRejectsBadLengths) {
    std::span<const u8> body;
    u32 used = 0;
    const u8 zero[16] = {0};
    EXPECT_EQ(frame_split(zero, zero + 16, kMaxFrame, body, used), FrameStatus::Bad);
    const u8 long_len[16] = {0x80, 0x80, 0x80, 0x01};  // 4-byte length
    EXPECT_EQ(frame_split(long_len, long_len + 16, kMaxFrame, body, used), FrameStatus::Bad);
    EXPECT_EQ(frame_split(long_len, long_len + 3, kMaxFrame, body, used), FrameStatus::Bad);
    const u8 big[16] = {0xFF, 0x7F};  // 16383 > limit 1024
    EXPECT_EQ(frame_split(big, big + 16, 1024, body, used), FrameStatus::Bad);
}

TEST(Frame, InflateRejectsLies) {
    std::span<const u8> body;
    const u8 below_threshold[] = {10, 1, 2, 3};  // claims 10 inflated bytes with threshold 256
    EXPECT_FALSE(frame_inflate(below_threshold, 256, body));
    const u8 garbage[] = {0x80, 0x04, 1, 2, 3, 4};  // claims 512 bytes, not a zlib stream
    EXPECT_FALSE(frame_inflate(garbage, 256, body));
    const u8 empty_stored[] = {0};
    EXPECT_FALSE(frame_inflate(empty_stored, 256, body));
}

namespace {
struct Tiny {  // a packet of n bytes that promises at most 20
    static constexpr u32 kId = 0x36;
    u8 n;
    usize bound() const { return 20; }
    u8* encode(u8* p) const {
        std::memset(p, 0xAB, n);
        return p + n;
    }
};
struct Brim {  // the largest bound that still gets the short header with the marker, used in full
    static constexpr u32 kId = 0x01;
    usize bound() const { return kShortBody - 2; }
    u8* encode(u8* p) const { return p + (kShortBody - 2); }
};
}  // namespace

TEST(Frame, TinyPacketsGetAOneByteLength) {
    // A packet whose bound says it stays tiny is framed the way vanilla frames it: one length
    // byte, not the padded three. (Every byte is paid for in AES once per viewer.)
    base::ByteBuf out;
    write_packet(out, kNoCompression, Tiny{9});
    ASSERT_EQ(out.len, 1u + 1 + 9);
    EXPECT_EQ(out.data[0], 10);  // length: id + payload
    EXPECT_EQ(out.data[1], 0x36);
    out.clear();
    write_packet(out, 256, Tiny{9});
    ASSERT_EQ(out.len, 1u + 1 + 1 + 9);
    EXPECT_EQ(out.data[0], 11);  // length: marker + id + payload
    EXPECT_EQ(out.data[1], 0);   // stored, not deflated
    EXPECT_EQ(out.data[2], 0x36);
    std::span<const u8> frame, body;
    u32 used = 0;
    ASSERT_EQ(frame_split(out.data, out.data + out.len, kMaxFrame, frame, used), FrameStatus::Ok);
    EXPECT_EQ(used, out.len);
    ASSERT_TRUE(frame_inflate(frame, 256, body));
    EXPECT_EQ(body.size(), 10u);

    // A threshold its bound could reach: back to the long header, and compressed when it does.
    for (const u8 n : {u8{3}, u8{20}}) {
        out.clear();
        write_packet(out, 16, Tiny{n});
        ASSERT_EQ(frame_split(out.data, out.data + out.len, kMaxFrame, frame, used), FrameStatus::Ok);
        EXPECT_EQ(used, out.len);
        EXPECT_EQ(frame[0] == 0, n + 1 < 16) << "stored below the threshold, deflated from it on";
        ASSERT_TRUE(frame_inflate(frame, 16, body));
        ASSERT_EQ(body.size(), n + 1u);
        EXPECT_EQ(body[0], 0x36);
    }
    // The largest bound that still fits one byte with the marker, filled to the brim.
    out.clear();
    write_packet(out, 256, Brim{});
    EXPECT_EQ(out.data[0], 126);
    ASSERT_EQ(frame_split(out.data, out.data + out.len, kMaxFrame, frame, used), FrameStatus::Ok);
    EXPECT_EQ(used, out.len);
}
