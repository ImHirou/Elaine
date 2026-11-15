#include "proto/frame.hpp"

#include <libdeflate.h>

#include "proto/wire.hpp"

namespace proto {

namespace {

// libdeflate (de)compressors are not thread-safe but are reusable: one per thread, forever.
// Level 4 is the knee for chunk data: ~2x zlib-6 speed at nearly the same ratio.
struct Codec {
    libdeflate_compressor* c = libdeflate_alloc_compressor(4);
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    // Separate scratch per direction: an inflated inbound body must stay valid while the handler
    // that is reading it writes (and maybe compresses) its replies.
    base::ByteBuf deflate_tmp{1u << 16};
    base::ByteBuf inflate_tmp{1u << 16};
    ~Codec() {
        libdeflate_free_compressor(c);
        libdeflate_free_decompressor(d);
    }
};

Codec& codec() {
    static thread_local Codec tl;
    return tl;
}

}  // namespace

void frame_compress(base::ByteBuf& out, u32 mark, u32 n) {
    Codec& k = codec();
    const usize bound = libdeflate_zlib_compress_bound(k.c, n);
    k.deflate_tmp.clear();
    u8* tmp = k.deflate_tmp.reserve(bound);
    const usize csize = libdeflate_zlib_compress(k.c, out.data + mark + 4, n, tmp, bound);
    DCHECK(csize != 0);

    // Rewrite in place: [len:3][usize][deflate]. The compressed form may be larger than the raw
    // one for incompressible data, so re-reserve from the mark.
    out.len = mark;
    u8* p = out.reserve(3 + kVarIntMaxBytes + csize);
    const u32 frame_len = varint_size(n) + static_cast<u32>(csize);
    DCHECK(frame_len <= kMaxFrame);
    put_varint3(p, frame_len);
    p = put_varint(p + 3, n);
    p = put_bytes(p, tmp, csize);
    out.commit(p);
}

void frame_raw(base::ByteBuf& out, i32 threshold, std::span<const u8> body) {
    FrameMark m;
    u8* p = frame_begin(out, threshold, body.size(), m);
    p = put_bytes(p, body.data(), body.size());
    frame_end(out, threshold, m, p);
}

bool frame_inflate(std::span<const u8> frame, i32 threshold, std::span<const u8>& body) {
    Reader r(frame);
    const u32 usize_ = r.varint();
    if (!r.ok) return false;
    if (usize_ == 0) {
        body = {r.p, r.remaining()};
        return !body.empty();
    }
    // Same validation as vanilla: a deflated packet below the threshold is a protocol violation.
    if (usize_ < static_cast<u32>(threshold) || usize_ > kMaxInflated) return false;
    Codec& k = codec();
    k.inflate_tmp.clear();
    u8* dst = k.inflate_tmp.reserve(usize_);
    usize actual = 0;
    if (libdeflate_zlib_decompress(k.d, r.p, r.remaining(), dst, usize_, &actual) != LIBDEFLATE_SUCCESS || actual != usize_)
        return false;
    body = {dst, usize_};
    return true;
}

}  // namespace proto
