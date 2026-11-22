// SPDX-License-Identifier: GPL-3.0-or-later
// Packet framing and compression.
//
// Uncompressed stream:  [VarInt len][body]
// Compressed stream:    [VarInt len][VarInt usize][zlib(body)]   usize == 0 -> body stored raw
// where body = [VarInt packet id][payload].
//
// Outbound frames are written in place: reserve the header, let the encoder write the body,
// then patch the length. The length is a fixed 3-byte (padded) VarInt, except for packets whose
// bound says they stay tiny: those get the one-byte length vanilla would write. Entity movement
// is such a packet, and every byte of it is paid for once per viewer in AES-CFB8 (one block per
// byte): the two bytes saved are ~15% of a move_entity_pos. Only bodies >= threshold take the
// (cold-ish) compress path.
#pragma once

#include <span>

#include "base/buf.hpp"
#include "proto/varint.hpp"

namespace proto {

inline constexpr u32 kMaxFrame = (1u << 21) - 1;  // 3-byte VarInt limit on any frame
inline constexpr u32 kMaxInflated = 1u << 21;     // what we accept from clients after inflate
inline constexpr i32 kNoCompression = -1;
// Largest body bound that gets a one-byte frame length (the length may also count the "stored"
// marker byte, and must stay below 128).
inline constexpr usize kShortBody = 126;

// Body reached `threshold`: replace out[mark..] (header + raw body of n bytes) with a compressed frame.
void frame_compress(base::ByteBuf& out, u32 mark, u32 n);

struct FrameMark {
    u32 at;
    u32 hdr;  // bytes before the body: length (1 or 3) + the "stored" marker when compression is on
};

// Reserve room for one packet whose body is at most `body_bound` bytes; returns where the body starts.
// `body_bound` is a constant for most packets, so the choice of header folds away at compile time.
ALWAYS_INLINE u8* frame_begin(base::ByteBuf& out, i32 threshold, usize body_bound, FrameMark& m) {
    m.at = out.len;
    // A body that could reach the threshold must keep the long header: compressing rewrites it.
    const bool small = body_bound <= kShortBody && (threshold < 0 || body_bound < static_cast<usize>(threshold));
    m.hdr = (small ? 1u : 3u) + (threshold >= 0 ? 1u : 0u);
    return out.reserve(body_bound + 8) + m.hdr;
}

ALWAYS_INLINE void frame_end(base::ByteBuf& out, i32 threshold, FrameMark m, const u8* body_end) {
    u8* start = out.data + m.at;
    const u32 n = static_cast<u32>(body_end - (start + m.hdr));
    if (m.hdr <= 2) {  // short: [len][body] or [len][0][body]
        DCHECK(n + m.hdr - 1 < 128);
        start[0] = static_cast<u8>(n + m.hdr - 1);
        if (m.hdr == 2) start[1] = 0;  // usize 0: stored
        out.commit(body_end);
        return;
    }
    if (UNLIKELY(n + 1 > kMaxFrame)) {
        // No frame can hold it. Whoever writes packets of stacks keeps them under the limit (a
        // menu goes slot by slot, equipment too); should one get here all the same, a client
        // that misses a packet is better off than one that is sent a frame it cannot read.
        DCHECK(false);
        out.len = m.at;
        return;
    }
    if (threshold < 0) {
        put_varint3(start, n);
        out.commit(body_end);
    } else if (LIKELY(n < static_cast<u32>(threshold))) {
        put_varint3(start, n + 1);
        start[3] = 0;  // usize 0: stored
        out.commit(body_end);
    } else {
        out.commit(body_end);
        frame_compress(out, m.at, n);
    }
}

// Frame an already-encoded body (id + payload), e.g. the generated registry/tag blobs.
void frame_raw(base::ByteBuf& out, i32 threshold, std::span<const u8> body);

// One frame of a packet that is the same for all its recipients but for a number in front: the
// id, that number as a VarInt, then `tail`. The frame is stored, never deflated, whatever its
// size - a length of 0 means "not compressed" to CompressionDecoder, which holds only deflated
// frames against the threshold. Deflating is per frame; here that would be per recipient.
inline void frame_stored(base::ByteBuf& out, i32 threshold, u32 id, u32 head, std::span<const u8> tail) {
    const u32 marker = threshold >= 0 ? 1u : 0u;
    u8* const start = out.reserve(3 + 1 + 2 * kVarIntMaxBytes + tail.size());
    u8* p = start + 3 + marker;
    p = put_varint(p, id);
    p = put_varint(p, head);
    std::memcpy(p, tail.data(), tail.size());
    p += tail.size();
    DCHECK(static_cast<u32>(p - (start + 3)) <= kMaxFrame);
    put_varint3(start, static_cast<u32>(p - (start + 3)));
    if (marker) start[3] = 0;
    out.commit(p);
}

// A packet type P provides:  static constexpr u32 kId;  usize bound() const;  u8* encode(u8*) const;
template <class P>
ALWAYS_INLINE void write_packet(base::ByteBuf& out, i32 threshold, const P& pkt) {
    FrameMark m;
    u8* p = frame_begin(out, threshold, pkt.bound() + 2, m);
    p = put_varint(p, P::kId);
    p = pkt.encode(p);
    frame_end(out, threshold, m, p);
}

// ---------------------------------------------------------------- inbound

enum class FrameStatus { Ok, NeedMore, Bad };

// Split one frame off [p, end). On Ok: `body` is the raw frame contents (after the length) and
// `consumed` the total bytes including the length prefix.
ALWAYS_INLINE FrameStatus frame_split(const u8* p, const u8* end, u32 max_len, std::span<const u8>& body, u32& consumed) {
    u32 len = 0;
    u32 n;
    if (LIKELY(end - p >= 8)) {
        n = get_varint_fast(p, len);
        if (UNLIKELY(n == 0 || n > 3)) return FrameStatus::Bad;
    } else {
        n = get_varint_slow(p, end, len);
        if (n == 0) return (end - p >= 3) ? FrameStatus::Bad : FrameStatus::NeedMore;
        if (n > 3) return FrameStatus::Bad;
    }
    if (UNLIKELY(len == 0 || len > max_len)) return FrameStatus::Bad;
    if (static_cast<usize>(end - p) < n + len) return FrameStatus::NeedMore;
    body = {p + n, len};
    consumed = n + len;
    return FrameStatus::Ok;
}

// Turn a compressed-stream frame body into the packet body. Raw bodies are returned in place;
// deflated ones are inflated into thread-local scratch (valid until the next call on this thread).
// Returns false on a malformed or oversized frame.
bool frame_inflate(std::span<const u8> frame, i32 threshold, std::span<const u8>& body);

}  // namespace proto
