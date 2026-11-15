#include "nbt/nbt.hpp"

#include <algorithm>
#include <cstdint>

#include "proto/wire.hpp"

namespace nbt {

namespace {

// What NbtAccounter is charged for a tag before its contents: the estimates in each TagType's
// load (ByteTag 9, ..., StringTag 36, ListTag 36, CompoundTag 48, the arrays 24).
constexpr u8 kWeight[kTypeCount] = {8, 9, 10, 12, 16, 12, 16, 24, 36, 36, 48, 24, 24};
constexpr u64 kKeyWeight = 28;    // CompoundTag's readString
constexpr u64 kEntryWeight = 36;  // ...and the map entry of a name not seen before
constexpr u64 kSlotWeight = 4;    // a list's reference to an element

// DataInputStream.readUTF over `n` bytes: sequences of one, two and three bytes, each checked
// for its shape only - not for being the shortest way to write its character. Returns the
// number of UTF-16 units, or -1 where readUTF throws. `plain` is cleared when writeUTF would
// write the same string differently: a raw NUL (it writes C0 80), a character in more bytes
// than it needs.
i32 scan_mutf8(const u8* p, u32 n, bool& plain) {
    u32 i = 0;
    // Names and most strings are ASCII: eight bytes at a time while none has its high bit set
    // and none is zero.
    while (i + 8 <= n) {
        const u64 w = load_le<u64>(p + i);
        if ((w | (w - 0x0101010101010101ull)) & 0x8080808080808080ull) break;
        i += 8;
    }
    u32 units = i;
    while (i < n) {
        const u8 c = p[i];
        if (c < 0x80) {
            plain &= c != 0;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 2 > n || (p[i + 1] & 0xC0) != 0x80) return -1;
            const u32 ch = static_cast<u32>(c & 0x1F) << 6 | (p[i + 1] & 0x3F);
            plain &= ch >= 0x80 || ch == 0;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 3 > n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return -1;
            const u32 ch = static_cast<u32>(c & 0x0F) << 12 | static_cast<u32>(p[i + 1] & 0x3F) << 6 | (p[i + 2] & 0x3F);
            plain &= ch >= 0x800;
            i += 3;
        } else {
            return -1;  // a continuation byte out of place, or the four-byte form Java does not use
        }
        ++units;
    }
    return static_cast<i32>(units);
}

// (int) and (long) of a double in Java: toward zero, saturating, NaN to 0.
i32 java_int(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return static_cast<i32>(v);
}
i64 java_long(double v) {
    if (v != v) return 0;
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return static_cast<i64>(v);
}

}  // namespace

// ---------------------------------------------------------------------------- reading

struct Doc::Parser {
    Doc& doc;
    const u8* base;
    u32 end;  // bytes there are
    u32 pos = 0;
    u32 depth = 0;  // compounds and lists around the tag being read
    u32 max_depth;
    u64 left;  // of the quota
    Error err = Error::kNone;

    bool fail(Error e) {
        err = e;
        return false;
    }
    ALWAYS_INLINE bool need(u64 n) { return LIKELY(end - pos >= n) || fail(Error::kTruncated); }
    ALWAYS_INLINE bool charge(u64 n) {
        if (UNLIKELY(n > left)) return fail(Error::kTooBig);
        left -= n;
        return true;
    }

    // A length and that many bytes of modified UTF-8; charges two bytes per UTF-16 unit, as the
    // String it becomes in Java.
    bool string(u32& len) {
        if (!need(2)) return false;
        len = load_be16(base + pos);
        if (!need(2 + len)) return false;
        const i32 units = scan_mutf8(base + pos + 2, len, doc.plain_);
        if (units < 0) return fail(Error::kBadString);
        pos += 2 + len;
        return charge(2ull * static_cast<u32>(units));
    }

    // The payload of a tag of `type` at pos: its record, then (compounds, lists) those of what it holds.
    bool value(u8 type, u32 name_len) {
        const u32 index = static_cast<u32>(doc.nodes_.size());
        doc.nodes_.push_back({pos, 0, 0, 0, static_cast<u16>(name_len), type, 0});
        if (!charge(kWeight[type])) return false;
        u32 count = 0;
        u8 aux = 0;
        switch (type) {
            case kByte:
            case kShort:
            case kInt:
            case kLong:
            case kFloat:
            case kDouble:
                if (!need(kFixedSize[type])) return false;
                pos += kFixedSize[type];
                break;
            case kByteArray:
            case kIntArray:
            case kLongArray: {
                if (!need(4)) return false;
                const i32 n = static_cast<i32>(load_be32(base + pos));
                if (n < 0) return fail(Error::kBadLength);  // new byte[-1]
                const u64 bytes = static_cast<u64>(n) * (type == kByteArray ? 1 : type == kIntArray ? 4 : 8);
                if (!need(4 + bytes) || !charge(bytes)) return false;
                pos += 4 + static_cast<u32>(bytes);
                count = static_cast<u32>(n);
                break;
            }
            case kString:
                if (!string(count)) return false;
                break;
            case kList: {
                if (depth >= max_depth) return fail(Error::kTooDeep);
                if (!need(5)) return false;
                aux = base[pos];
                const i32 n = static_cast<i32>(load_be32(base + pos + 1));
                if (n < 0) return fail(Error::kBadLength);
                if (aux == kEnd && n > 0) return fail(Error::kBadType);  // "Missing type on ListTag"
                pos += 5;
                count = static_cast<u32>(n);
                if (n == 0) break;  // an empty list may name any type, one that does not exist included
                if (aux >= kTypeCount) return fail(Error::kBadType);
                if (const u32 each = kFixedSize[aux]) {  // numbers: the bytes are the index
                    const u64 bytes = static_cast<u64>(count) * each;
                    if (!need(bytes) || !charge(static_cast<u64>(count) * (kSlotWeight + kWeight[aux]))) return false;
                    pos += static_cast<u32>(bytes);
                    break;
                }
                // Every other element takes a byte at least: a count the input cannot hold stops here.
                if (!need(count) || !charge(kSlotWeight * count)) return false;
                ++depth;
                for (u32 i = 0; i < count; ++i)
                    if (!value(aux, 0)) return false;
                --depth;
                break;
            }
            case kCompound: {
                if (depth >= max_depth) return fail(Error::kTooDeep);
                ++depth;
                bool unnamed = true;
                for (;;) {
                    if (!need(1)) return false;
                    const u8 of = base[pos++];
                    if (of == kEnd) break;
                    u32 len;
                    if (!string(len) || !charge(kKeyWeight + kEntryWeight)) return false;
                    if (of >= kTypeCount) return fail(Error::kBadType);
                    unnamed &= len == 0;
                    if (!value(of, len)) return false;
                    ++count;
                }
                --depth;
                aux = count > 0 && unnamed;
                break;
            }
            default: return fail(Error::kBadType);
        }
        Node& n = doc.nodes_[index];
        n.end = pos;
        n.next = static_cast<u32>(doc.nodes_.size());
        n.count = count;
        n.aux = aux;
        return true;
    }
};

Error Doc::parse(std::span<const u8> bytes, Form form, const Limits& limits) {
    nodes_.clear();
    base_ = bytes.data();
    size_ = 0;
    weight_ = 0;
    plain_ = true;
    if (bytes.size() > UINT32_MAX) return Error::kTooBig;  // offsets are 32 bits
    Parser p{*this, base_, static_cast<u32>(bytes.size()), 0, 0, limits.depth, limits.quota};
    if (!p.need(1)) return p.err;
    const u8 type = base_[p.pos++];
    if (type == kEnd) {  // TAG_End alone: vanilla's "no tag"
        size_ = 1;
        return Error::kNone;
    }
    if (type >= kTypeCount) return Error::kBadType;
    if (form == Form::kNamed) {
        if (!p.need(2)) return p.err;
        const u32 len = load_be16(base_ + p.pos);
        if (!p.need(2 + len)) return p.err;
        p.pos += 2 + len;
    }
    if (!p.value(type, 0)) {
        nodes_.clear();
        return p.err;
    }
    size_ = p.pos;
    weight_ = limits.quota - p.left;
    return Error::kNone;
}

i64 Tag::as_long() const {
    const u8* p = payload();
    switch (type) {
        case kByte: return static_cast<i8>(*p);
        case kShort: return static_cast<i16>(load_be16(p));
        case kInt: return static_cast<i32>(load_be32(p));
        case kLong: return static_cast<i64>(load_be64(p));
        case kFloat: return java_long(std::bit_cast<float>(load_be32(p)));
        case kDouble: return java_long(std::bit_cast<double>(load_be64(p)));
        default: return 0;
    }
}

i32 Tag::as_int() const {
    const u8* p = payload();
    switch (type) {
        case kFloat: return java_int(std::bit_cast<float>(load_be32(p)));
        case kDouble: return java_int(std::bit_cast<double>(load_be64(p)));
        default: return static_cast<i32>(as_long());  // (int) of a long keeps the low bits
    }
}

double Tag::as_double() const {
    const u8* p = payload();
    switch (type) {
        case kFloat: return std::bit_cast<float>(load_be32(p));
        case kDouble: return std::bit_cast<double>(load_be64(p));
        default: return static_cast<double>(as_long());
    }
}

Tag Tag::get(std::string_view key) const {
    if (type != kCompound) return {};
    Tag found;
    for (const Tag e : entries())
        if (e.name() == key) found = e;
    return found;
}

// ---------------------------------------------------------------------------- writing a loaded tag back

void Doc::write(Tag tag, base::ByteBuf& out) {
    if (tag.type == kCompound) return write_compound(tag, out);
    if (tag.type == kList) return write_list(tag, out);
    const auto b = tag.bytes();
    out.append(b.data(), b.size());
}

void Doc::write_compound(Tag tag, base::ByteBuf& out) {
    const usize mark = order_.size();
    for (const Tag e : tag.entries()) order_.push_back(e.at);
    const auto name = [&](u32 node) { return Tag{this, node, kCompound, false}.name(); };
    std::sort(order_.begin() + static_cast<std::ptrdiff_t>(mark), order_.end(), [&](u32 a, u32 b) {
        const int c = name(a).compare(name(b));
        return c != 0 ? c < 0 : a < b;
    });
    // Of the values read for one name the map keeps the last.
    usize kept = mark;
    for (usize i = mark; i < order_.size(); ++i) {
        if (i + 1 < order_.size() && name(order_[i]) == name(order_[i + 1])) continue;
        order_[kept++] = order_[i];
    }
    order_.resize(kept);
    for (usize i = mark; i < kept; ++i) {  // by index: the entries below push their own onto order_
        const Node& n = nodes_[order_[i]];
        const Tag entry{this, order_[i], static_cast<Type>(n.type), false};
        const std::string_view key = entry.name();
        u8* p = out.reserve(3 + key.size());
        *p = n.type;
        store_be16(p + 1, static_cast<u16>(key.size()));
        std::memcpy(p + 3, key.data(), key.size());
        out.len += 3 + static_cast<u32>(key.size());
        write(entry, out);
    }
    order_.resize(mark);
    *out.reserve(1) = kEnd;
    out.len += 1;
}

// ListTag.write: the element type is that of the elements if they agree, TAG_Compound if they
// do not; in a list of compounds everything that is not a compound goes into {"": x}, and so
// does a compound that would itself be taken for such a wrapper when read back.
void Doc::write_list(Tag tag, base::ByteBuf& out) {
    const Node& n = nodes_[tag.at];
    Type of = kEnd;
    for (const Tag e : tag.elements()) {
        if (of == kEnd) {
            of = e.type;
        } else if (of != e.type) {
            of = kCompound;
            break;
        }
        if (e.flat) break;  // numbers as stored: all of one type
    }
    u8* p = out.reserve(5);
    *p = of;
    store_be32(p + 1, n.count);
    out.len += 5;
    if (n.count && kFixedSize[n.aux]) {
        out.append(base_ + n.pos + 5, n.end - n.pos - 5);
        return;
    }
    for (const Tag e : tag.elements()) {
        if (of == kCompound && (e.type != kCompound || e.wrapper())) {
            u8* w = out.reserve(3);
            w[0] = e.type;
            w[1] = w[2] = 0;  // the name: ""
            out.len += 3;
            write(e, out);
            *out.reserve(1) = kEnd;
            out.len += 1;
        } else {
            write(e, out);
        }
    }
}

// ---------------------------------------------------------------------------- Writer

void Writer::put_string(std::string_view utf8) {
    u8* p = open(2 + proto::mutf8_bound(utf8));
    u8* end = proto::put_mutf8(p + 2, utf8);
    if (UNLIKELY(end - (p + 2) > static_cast<std::ptrdiff_t>(kMaxStringBytes))) {
        DCHECK(false);  // writeUTF throws here; nothing the server writes comes near
        end = p + 2;
    }
    store_be16(p, static_cast<u16>(end - (p + 2)));
    b_.commit(end);
}

void Writer::put_string(std::string_view name, std::string_view utf8) {
    b_.commit(header(open(3 + name.size()), kString, name));
    put_string(utf8);
}

void Writer::put_string_raw(std::string_view mutf8) {
    DCHECK(mutf8.size() <= kMaxStringBytes);
    u8* p = open(2 + mutf8.size());
    store_be16(p, static_cast<u16>(mutf8.size()));
    std::memcpy(p + 2, mutf8.data(), mutf8.size());
    b_.commit(p + 2 + mutf8.size());
}

void Writer::put_string_raw(std::string_view name, std::string_view mutf8) {
    b_.commit(header(open(3 + name.size()), kString, name));
    put_string_raw(mutf8);
}

void Writer::put_byte_array(std::string_view name, std::span<const i8> v) {
    u8* p = header(open(3 + name.size() + 4 + v.size()), kByteArray, name);
    store_be32(p, static_cast<u32>(v.size()));
    std::memcpy(p + 4, v.data(), v.size());
    b_.commit(p + 4 + v.size());
}

void Writer::put_int_array(std::string_view name, std::span<const i32> v) {
    u8* p = header(open(3 + name.size() + 4 + v.size() * 4), kIntArray, name);
    store_be32(p, static_cast<u32>(v.size()));
    p += 4;
    for (const i32 x : v) {
        store_be32(p, static_cast<u32>(x));
        p += 4;
    }
    b_.commit(p);
}

void Writer::put_long_array(std::string_view name, std::span<const i64> v) {
    u8* p = header(open(3 + name.size() + 4 + v.size() * 8), kLongArray, name);
    store_be32(p, static_cast<u32>(v.size()));
    p += 4;
    for (const i64 x : v) {
        store_be64(p, static_cast<u64>(x));
        p += 8;
    }
    b_.commit(p);
}

void Writer::begin_list(Type element, u32 count) {
    u8* p = open(5);
    *p = element;
    store_be32(p + 1, count);
    b_.commit(p + 5);
}

void Writer::begin_list(std::string_view name, Type element, u32 count) {
    b_.commit(header(open(3 + name.size()), kList, name));
    begin_list(element, count);
}

void Writer::put_raw(std::string_view name, Type type, std::span<const u8> payload) {
    u8* p = header(open(3 + name.size() + payload.size()), type, name);
    std::memcpy(p, payload.data(), payload.size());
    b_.commit(p + payload.size());
}

}  // namespace nbt
