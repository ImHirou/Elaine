// SPDX-License-Identifier: GPL-3.0-or-later
// The packets chat travels in, field by field, as the codecs of the 26.3 jar lay them out:
// ServerboundChatPacket, ServerboundChatAckPacket, ServerboundChatSessionUpdatePacket,
// LastSeenMessages.Update, ClientboundPlayerChatPacket (SignedMessageBody.Packed, FilterMask,
// ChatType.Bound), ClientboundDisguisedChatPacket. The bytes are written out by hand.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "proto/frame.hpp"
#include "proto/packets.hpp"

using namespace proto;

namespace {

using Bytes = std::vector<u8>;

Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
Bytes vi(u32 v) {
    u8 buf[8];
    return {buf, put_varint(buf, v)};
}
Bytes be64(u64 v) {
    Bytes out(8);
    for (u32 i = 0; i < 8; ++i) out[i] = static_cast<u8>(v >> (56 - 8 * i));
    return out;
}
Bytes str(std::string_view s) { return vi(static_cast<u32>(s.size())) + Bytes(s.begin(), s.end()); }

template <class P>
Bytes encoded(const P& p) {
    base::ByteBuf buf;
    u8* end = p.encode(buf.reserve(p.bound()));
    EXPECT_LE(static_cast<usize>(end - buf.data), p.bound());
    return {buf.data, end};
}

const Bytes kName{8, 0, 5, 'S', 't', 'e', 'v', 'e'};  // a component that is a string: TAG_String "Steve"
const Bytes kAlex{8, 0, 4, 'A', 'l', 'e', 'x'};
const Bytes kShown{8, 0, 2, 'h', 'i'};

}  // namespace

TEST(ChatPackets, WhatAPlayerTyped) {
    // message, timestamp (ms), salt, no signature, then what it has seen: offset, 20 bits in
    // three bytes, a checksum.
    const Bytes unsigned_chat =
        str("hello") + be64(1700000000123ull) + be64(0x1122334455667788ull) + Bytes{0} + vi(0) + Bytes{0, 0, 0} + Bytes{1};
    Reader r(unsigned_chat.data(), unsigned_chat.size());
    sb::Chat c = sb::Chat::decode(r);
    EXPECT_TRUE(r.done());
    EXPECT_EQ(c.message, "hello");
    EXPECT_EQ(c.timestamp, 1700000000123ll);
    EXPECT_EQ(c.salt, 0x1122334455667788ll);
    EXPECT_FALSE(c.has_signature);
    EXPECT_EQ(c.last_seen.offset, 0u);
    EXPECT_EQ(c.last_seen.acknowledged, 0u);
    EXPECT_EQ(c.last_seen.checksum, 1);

    // With a signature - 256 bytes and no length - and a window that has moved: bit 0 is the
    // low bit of the first byte, bit 19 bit 3 of the third.
    const Bytes signed_chat =
        str("hi") + be64(5) + be64(6) + Bytes{1} + Bytes(256, 0xAB) + vi(300) + Bytes{0x01, 0x80, 0x08} + Bytes{0x7F};
    Reader s(signed_chat.data(), signed_chat.size());
    c = sb::Chat::decode(s);
    EXPECT_TRUE(s.done());
    EXPECT_EQ(c.message, "hi");
    EXPECT_TRUE(c.has_signature);
    EXPECT_EQ(c.last_seen.offset, 300u);
    EXPECT_EQ(c.last_seen.acknowledged, 1u | 1u << 15 | 1u << 19);
    EXPECT_EQ(c.last_seen.checksum, 0x7F);

    // 256 characters are a message, 257 are not - counted as Java counts them: a character
    // outside the BMP is two.
    const auto takes = [](const std::string& message) {
        const Bytes in = str(message) + Bytes(16, 0) + Bytes{0} + vi(0) + Bytes{0, 0, 0, 0};
        Reader rd(in.data(), in.size());
        sb::Chat::decode(rd);
        return rd.done();
    };
    EXPECT_TRUE(takes(std::string(256, 'a')));
    EXPECT_FALSE(takes(std::string(257, 'a')));
    std::string umlauts, emoji;
    for (u32 i = 0; i < 256; ++i) umlauts += "\xC3\xA4";        // 256 x U+00E4: two bytes, one char each
    for (u32 i = 0; i < 128; ++i) emoji += "\xF0\x9F\x98\x80";  // 128 x U+1F600: four bytes, two chars each
    EXPECT_TRUE(takes(umlauts));
    EXPECT_FALSE(takes(umlauts + "a"));
    EXPECT_TRUE(takes(emoji));
    EXPECT_FALSE(takes(emoji + "a"));
    EXPECT_EQ(utf16_length("a\xC3\xA4\xE2\x82\xAC\xF0\x9F\x98\x80"), 5u);

    // Cut off anywhere, it is no packet.
    for (usize n = 0; n < unsigned_chat.size(); ++n) {
        Reader cut(unsigned_chat.data(), n);
        sb::Chat::decode(cut);
        EXPECT_FALSE(cut.done()) << n;
    }
}

TEST(ChatPackets, AckAndSession) {
    const Bytes ack = vi(70);
    Reader a(ack.data(), ack.size());
    EXPECT_EQ(sb::ChatAck::decode(a).offset, 70u);
    EXPECT_TRUE(a.done());

    // RemoteChatSession.Data: the session's id; ProfilePublicKey.Data: expiry (ms), the key and
    // its signature as byte arrays.
    const Bytes session = be64(0x0102030405060708ull) + be64(0x1112131415161718ull) + be64(1800000000000ull) + vi(294) +
                          Bytes(294, 7) + vi(512) + Bytes(512, 9);
    Reader s(session.data(), session.size());
    const sb::ChatSessionUpdate u = sb::ChatSessionUpdate::decode(s);
    EXPECT_TRUE(s.done());
    EXPECT_EQ(u.session.hi, 0x0102030405060708ull);
    EXPECT_EQ(u.session.lo, 0x1112131415161718ull);
    EXPECT_EQ(u.expires_at, 1800000000000ll);
    // A key longer than 512 bytes, a signature longer than 4096: not this packet.
    const Bytes long_key = Bytes(24, 0) + vi(513) + Bytes(513, 7) + vi(1) + Bytes{0};
    Reader k(long_key.data(), long_key.size());
    sb::ChatSessionUpdate::decode(k);
    EXPECT_FALSE(k.ok);
    const Bytes long_signature = Bytes(24, 0) + vi(1) + Bytes{0} + vi(4097) + Bytes(4097, 7);
    Reader g(long_signature.data(), long_signature.size());
    sb::ChatSessionUpdate::decode(g);
    EXPECT_FALSE(g.ok);

    // ChatVisiblity.BY_ID wraps.
    EXPECT_EQ(sb::ClientInformation::visibility(0), sb::ClientInformation::kChatFull);
    EXPECT_EQ(sb::ClientInformation::visibility(1), sb::ClientInformation::kChatSystem);
    EXPECT_EQ(sb::ClientInformation::visibility(2), sb::ClientInformation::kChatHidden);
    EXPECT_EQ(sb::ClientInformation::visibility(3), sb::ClientInformation::kChatFull);
    EXPECT_EQ(sb::ClientInformation::visibility(static_cast<u32>(-1)), sb::ClientInformation::kChatHidden);
}

TEST(ChatPackets, PlayerChat) {
    const Uuid steve{0x0102030405060708ull, 0x1112131415161718ull};
    // Said by Steve, type 0 (holder: 1), nothing made of it by the server.
    const cb::PlayerChatTail plain{steve, "hello", 1700000000123ll, {}, {0, kName}};
    const Bytes tail = be64(steve.hi) + be64(steve.lo)                    // sender
                       + vi(0)                                            // index of the message in its sender's session
                       + Bytes{0}                                         // no signature
                       + str("hello") + be64(1700000000123ull) + be64(0)  // SignedMessageBody.Packed: content, time, salt...
                       + vi(0)                                            // ...and nothing seen
                       + Bytes{0}                                         // no unsigned content
                       + vi(0)                                            // FilterMask.PASS_THROUGH
                       + vi(1) + kName + Bytes{0};                        // ChatType.Bound: the type, the name, no target
    EXPECT_EQ(encoded(plain), tail);

    // A whisper (type 3) whose text the server changed: the component follows a true, and so
    // does the target's name.
    const cb::PlayerChatTail whisper{steve, "hi @p", 5, kShown, {3, kName, kAlex}};
    EXPECT_EQ(encoded(whisper), be64(steve.hi) + be64(steve.lo) + vi(0) + Bytes{0} + str("hi @p") + be64(5) + be64(0) + vi(0) +
                                    Bytes{1} + kShown + vi(0) + vi(4) + kName + Bytes{1} + kAlex);

    // The frame: a length in three bytes, "not compressed", the packet's id, this recipient's
    // count, the tail - also where the tail is longer than the threshold.
    for (const i32 threshold : {-1, 16, 256}) {
        base::ByteBuf out;
        frame_stored(out, threshold, cb::PlayerChatTail::kId, 300, tail);
        Reader r(out.data, out.len);
        u32 length = 0;
        ASSERT_EQ(get_varint(r.p, r.end, length), 3u);
        r.p += 3;
        EXPECT_EQ(length, r.remaining());
        if (threshold >= 0) {
            EXPECT_EQ(r.varint(), 0u);
        }
        EXPECT_EQ(r.varint(), static_cast<u32>(ids::play::cb::kPlayerChat));
        EXPECT_EQ(r.varint(), 300u);
        const auto rest = r.bytes(r.remaining());
        EXPECT_EQ(Bytes(rest.begin(), rest.end()), tail);
        // ...and it is a frame as every other: what splits frames takes it whole.
        std::span<const u8> body;
        u32 consumed = 0;
        ASSERT_EQ(frame_split(out.data, out.data + out.len, kMaxFrame, body, consumed), FrameStatus::Ok);
        EXPECT_EQ(consumed, out.len);
        std::span<const u8> packet = body;
        if (threshold >= 0) {  // (a stream that is not compressed has no marker to take off)
            ASSERT_TRUE(frame_inflate(body, threshold, packet));
        }
        EXPECT_EQ(packet.size(), vi(ids::play::cb::kPlayerChat).size() + 2 + tail.size());  // the id, 300 in two bytes
    }
}

TEST(ChatPackets, DisguisedChat) {
    const cb::DisguisedChat say{kShown, {4, kName}};
    EXPECT_EQ(encoded(say), kShown + vi(5) + kName + Bytes{0});
    const cb::DisguisedChat whisper{kShown, {2, kName, kAlex}};
    EXPECT_EQ(encoded(whisper), kShown + vi(3) + kName + Bytes{1} + kAlex);
}
