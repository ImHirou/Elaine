#include "proto/wire.hpp"

#include <string>

namespace proto {

u8* put_mutf8(u8* p, std::string_view s) {
    const u8* in = reinterpret_cast<const u8*>(s.data());
    const u8* end = in + s.size();
    while (in < end) {
        const u8 c = *in;
        if (c == 0) {
            *p++ = 0xC0;
            *p++ = 0x80;
            ++in;
        } else if (c < 0xF0 || end - in < 4) {
            *p++ = c;  // ASCII and 2/3-byte sequences are identical in modified UTF-8
            ++in;
        } else {
            // 4-byte UTF-8 -> UTF-16 surrogate pair, each half encoded as a 3-byte sequence.
            const u32 cp = (static_cast<u32>(c & 0x07) << 18) | (static_cast<u32>(in[1] & 0x3F) << 12) |
                           (static_cast<u32>(in[2] & 0x3F) << 6) | static_cast<u32>(in[3] & 0x3F);
            const u32 v = cp - 0x10000;
            const u32 hi = 0xD800 + (v >> 10);
            const u32 lo = 0xDC00 + (v & 0x3FF);
            for (u32 u : {hi, lo}) {
                *p++ = static_cast<u8>(0xE0 | (u >> 12));
                *p++ = static_cast<u8>(0x80 | ((u >> 6) & 0x3F));
                *p++ = static_cast<u8>(0x80 | (u & 0x3F));
            }
            in += 4;
        }
    }
    return p;
}

u8* put_nbt_text(u8* p, std::string_view s) {
    *p++ = 8;  // TAG_String
    u8* len_at = p;
    p += 2;
    u8* body = p;
    p = put_mutf8(p, s);
    store_be16(len_at, static_cast<u16>(p - body));
    return p;
}

void json_escape(std::string& out, std::string_view s) {
    static const char kHex[] = "0123456789abcdef";
    for (char ch : s) {
        const u8 c = static_cast<u8>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 15];
                } else {
                    out += ch;
                }
        }
    }
}

}  // namespace proto
