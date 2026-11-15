// Binary NBT: the tag tree of vanilla's saves and of everything structured in its protocol -
// text components, block entities, custom item data.
//
// Nothing here builds a tree of objects. A document stays the bytes it arrived as: reading one
// (Doc) checks it the way vanilla's loader does and leaves a tape - one small record per tag,
// numbers inside lists excepted - so that any tag is reached, and any subtree skipped, without
// walking bytes twice. Writing (Writer) appends to a ByteBuf.
//
// The loader is NbtIo and the TagTypes of 26.3:
//   * compounds and lists nest at most 512 deep (NbtAccounter.pushDepth);
//   * every tag is charged what vanilla takes its Java object to weigh, and a tag from a client
//     may weigh 2 MiB (NbtAccounter.defaultQuota) - which also bounds the tape;
//   * strings are Java's modified UTF-8, accepted as DataInput.readUTF accepts it;
//   * a compound's entry read twice keeps its last value (HashMap.put);
//   * a list element that is a compound holding nothing but a tag named "" stands for that tag
//     (ListTag.addAndUnwrap: how a list of mixed types travels).
//
// One thread per Doc. A Tag is a view: it lives as long as the Doc and the bytes it was parsed from.
#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "base/buf.hpp"
#include "base/types.hpp"

namespace nbt {

enum Type : u8 {
    kEnd = 0,
    kByte = 1,
    kShort = 2,
    kInt = 3,
    kLong = 4,
    kFloat = 5,
    kDouble = 6,
    kByteArray = 7,
    kString = 8,
    kList = 9,
    kCompound = 10,
    kIntArray = 11,
    kLongArray = 12,
};
inline constexpr u32 kTypeCount = 13;
inline constexpr u32 kMaxStringBytes = 65535;  // a string's length is an unsigned short

// Payload bytes of the types that have a fixed size; 0 for the rest.
inline constexpr u8 kFixedSize[kTypeCount] = {0, 1, 2, 4, 8, 4, 8, 0, 0, 0, 0, 0, 0};

// What a reader is willing to load (NbtAccounter).
struct Limits {
    u64 quota;  // accounted bytes
    u32 depth;  // nested compounds and lists
};
inline constexpr Limits kUntrusted{2097152, 512};              // defaultQuota(): whatever a client sends
inline constexpr Limits kTrusted{0x7FFFFFFFFFFFFFFFull, 512};  // unlimitedHeap(): files of our own

enum class Form : u8 {
    kNameless,  // type, payload: the network form (NbtIo.readAnyTag)
    kNamed,     // type, name, payload: files (NbtIo.readUnnamedTag - the name is skipped unread)
};

enum class Error : u8 {
    kNone,
    kTruncated,  // the input ends inside the tag
    kBadType,    // a type id that does not exist, or a list of TAG_End that is not empty
    kBadLength,  // a negative element count
    kBadString,  // not modified UTF-8
    kTooDeep,
    // Over the quota. The one answer that may be wrong: a repeated name in a compound is charged
    // as a new entry (vanilla charges it 36 bytes less), so that reading stays one pass.
    kTooBig,
};

class Doc;
struct Entries;
struct Elements;

struct Node {
    u32 pos;       // where the payload starts
    u32 end;       // ...and where it ends
    u32 next;      // the record after this tag and all it holds
    u32 count;     // entries of a compound, elements of a list or an array, bytes of a string
    u16 name_len;  // entry of a compound: its name is the name_len bytes before the payload
    u8 type;
    u8 aux;  // list: the element type as stored; compound: 1 if all it holds is named ""
};

struct Tag {
    const Doc* doc = nullptr;
    u32 at = 0;         // its record on the tape - or, for a number without one, its payload's offset
    Type type = kEnd;   // kEnd: no tag (the root of an empty document, a name get() did not find)
    bool flat = false;  // a number inside a list or an array

    explicit operator bool() const { return type != kEnd; }
    bool numeric() const { return type >= kByte && type <= kDouble; }
    // What NbtOps.getList takes: a list, or an array as a list of its numbers.
    bool collection() const { return type == kList || type == kByteArray || type == kIntArray || type == kLongArray; }

    inline const u8* payload() const;
    inline std::span<const u8> bytes() const;  // the payload, whole
    inline std::string_view name() const;      // of a compound's entry
    inline std::string_view str() const;       // kString: its bytes (modified UTF-8)
    inline u32 size() const;                   // entries of a compound; elements of a list or an array

    // A number as java.lang.Number hands it out (NumericTag.asNumber().longValue() and so on):
    // integers truncate, floats saturate, NaN is 0.
    i64 as_long() const;
    i32 as_int() const;
    double as_double() const;

    // kCompound: the entry of that name - the last one, if it comes twice - or no tag.
    Tag get(std::string_view key) const;
    inline Entries entries() const;
    // kList and the arrays: the elements as vanilla's ListTag holds them, wrappers taken off.
    inline Elements elements() const;
    inline Type element_type() const;  // kList: as stored in the bytes
    // kCompound: holds something, and all of it is named "" - what a list takes for a wrapped element.
    inline bool wrapper() const;
};

class Doc {
  public:
    // Reads one tag from the front of `bytes`. kNone: root() is the tag and size() the bytes it
    // took. Anything else: the document is empty. `bytes` must outlive the tags handed out.
    Error parse(std::span<const u8> bytes, Form form = Form::kNameless, const Limits& limits = kUntrusted);

    Tag root() const { return nodes_.empty() ? Tag{} : Tag{this, 0, static_cast<Type>(nodes_[0].type), false}; }
    u32 size() const { return size_; }
    u64 weight() const { return weight_; }  // NbtAccounter.getUsage() after the load
    // Every string and name is in the form DataOutput.writeUTF produces. Then two names are the
    // same Java string exactly when their bytes are equal, and vanilla writes a string back as
    // it read it. Vanilla's own files and clients never give anything else.
    bool plain_strings() const { return plain_; }

    // Appends the payload of `tag` as vanilla would write the tag it loaded: one value per name
    // in a compound, list elements wrapped only where the list is mixed, an empty list without
    // an element type (ListTag.write). The entries of a compound go out sorted by name - vanilla
    // writes them in HashMap order, which says nothing to a reader; sorted, equal compounds are
    // equal bytes. Strings are copied as they are: see plain_strings().
    void write(Tag tag, base::ByteBuf& out);

    const Node& node(u32 i) const { return nodes_[i]; }
    const u8* data() const { return base_; }

  private:
    struct Parser;
    void write_compound(Tag tag, base::ByteBuf& out);
    void write_list(Tag tag, base::ByteBuf& out);

    const u8* base_ = nullptr;
    u32 size_ = 0;
    u64 weight_ = 0;
    bool plain_ = true;
    std::vector<Node> nodes_;
    std::vector<u32> order_;  // write(): the entries of the compounds being written, innermost last
};

// ---------------------------------------------------------------------------- walking

struct Entries {
    const Doc* doc;
    u32 first, last;
    struct It {
        const Doc* doc;
        u32 i;
        Tag operator*() const { return {doc, i, static_cast<Type>(doc->node(i).type), false}; }
        It& operator++() {
            i = doc->node(i).next;
            return *this;
        }
        bool operator!=(const It& o) const { return i != o.i; }
    };
    It begin() const { return {doc, first}; }
    It end() const { return {doc, last}; }
};

struct Elements {
    const Doc* doc;
    u32 first, last;  // tape records - or, with a step, payload offsets
    u32 step;         // 0: every element has a record
    Type type;        // of the elements that have none
    struct It {
        const Doc* doc;
        u32 i, step;
        Type type;
        Tag operator*() const {
            if (step) return {doc, i, type, true};
            const Node& n = doc->node(i);
            if (n.type != kCompound || !n.aux) return {doc, i, static_cast<Type>(n.type), false};
            // {"": x} stands for x; of several tags named "" the last is the one a map keeps.
            u32 inner = i + 1;
            while (doc->node(inner).next != n.next) inner = doc->node(inner).next;
            return {doc, inner, static_cast<Type>(doc->node(inner).type), false};
        }
        It& operator++() {
            i = step ? i + step : doc->node(i).next;
            return *this;
        }
        bool operator!=(const It& o) const { return i != o.i; }
    };
    It begin() const { return {doc, first, step, type}; }
    It end() const { return {doc, last, step, type}; }
};

inline const u8* Tag::payload() const { return doc->data() + (flat ? at : doc->node(at).pos); }
inline std::span<const u8> Tag::bytes() const {
    if (flat) return {doc->data() + at, kFixedSize[type]};
    const Node& n = doc->node(at);
    return {doc->data() + n.pos, n.end - n.pos};
}
inline std::string_view Tag::name() const {
    const Node& n = doc->node(at);
    return {reinterpret_cast<const char*>(doc->data()) + n.pos - n.name_len, n.name_len};
}
inline std::string_view Tag::str() const {
    DCHECK(type == kString);
    const Node& n = doc->node(at);
    return {reinterpret_cast<const char*>(doc->data()) + n.pos + 2, n.count};
}
inline u32 Tag::size() const { return doc->node(at).count; }
inline Type Tag::element_type() const { return static_cast<Type>(doc->node(at).aux); }
inline bool Tag::wrapper() const { return type == kCompound && doc->node(at).aux != 0; }
inline Entries Tag::entries() const {
    DCHECK(type == kCompound);
    return {doc, at + 1, doc->node(at).next};
}
inline Elements Tag::elements() const {
    DCHECK(collection());
    const Node& n = doc->node(at);
    if (type != kList) {  // an array: a count, then the numbers
        const Type of = type == kByteArray ? kByte : type == kIntArray ? kInt : kLong;
        return {doc, n.pos + 4, n.end, kFixedSize[of], of};
    }
    if (n.count && kFixedSize[n.aux]) return {doc, n.pos + 5, n.end, kFixedSize[n.aux], static_cast<Type>(n.aux)};
    return {doc, at + 1, n.next, 0, kEnd};
}

// ---------------------------------------------------------------------------- writing
//
// Appends tags to a ByteBuf, one capacity check per tag. Nothing is verified: the caller ends
// the compounds it begins and gives a list the elements it announced.
//
//     Writer w(buf);
//     w.root(kCompound);              // a nameless root: the network form
//     w.put_string("id", "minecraft:chest");
//     w.begin_list("Items", kCompound, 2);
//     for (...) { w.put_byte("Slot", i); ...; w.end_compound(); }
//     w.end_compound();
//
// A method that takes a name writes an entry of the open compound; without one it writes a bare
// payload - an element of the open list, or what follows root().
class Writer {
  public:
    explicit Writer(base::ByteBuf& out) : b_(out) {}

    void root(Type type) { fixed<u8>(type); }
    void root(Type type, std::string_view name) { b_.commit(header(open(3 + name.size()), type, name)); }  // files

    void put_byte(std::string_view name, i8 v) { fixed<u8>(name, kByte, static_cast<u8>(v)); }
    void put_short(std::string_view name, i16 v) { fixed<u16>(name, kShort, static_cast<u16>(v)); }
    void put_int(std::string_view name, i32 v) { fixed<u32>(name, kInt, static_cast<u32>(v)); }
    void put_long(std::string_view name, i64 v) { fixed<u64>(name, kLong, static_cast<u64>(v)); }
    void put_float(std::string_view name, float v) { fixed<u32>(name, kFloat, std::bit_cast<u32>(v)); }
    void put_double(std::string_view name, double v) { fixed<u64>(name, kDouble, std::bit_cast<u64>(v)); }
    void put_byte(i8 v) { fixed<u8>(static_cast<u8>(v)); }
    void put_short(i16 v) { fixed<u16>(static_cast<u16>(v)); }
    void put_int(i32 v) { fixed<u32>(static_cast<u32>(v)); }
    void put_long(i64 v) { fixed<u64>(static_cast<u64>(v)); }
    void put_float(float v) { fixed<u32>(std::bit_cast<u32>(v)); }
    void put_double(double v) { fixed<u64>(std::bit_cast<u64>(v)); }

    // UTF-8 in, modified UTF-8 out. A string that does not fit 65535 bytes is written empty.
    void put_string(std::string_view name, std::string_view utf8);
    void put_string(std::string_view utf8);
    // A string that is modified UTF-8 already: one taken from a Doc.
    void put_string_raw(std::string_view name, std::string_view mutf8);
    void put_string_raw(std::string_view mutf8);

    void put_byte_array(std::string_view name, std::span<const i8> v);
    void put_int_array(std::string_view name, std::span<const i32> v);
    void put_long_array(std::string_view name, std::span<const i64> v);

    void begin_compound(std::string_view name) { b_.commit(header(open(3 + name.size()), kCompound, name)); }
    void begin_compound() {}  // as a list's element, a compound starts with its first entry
    void end_compound() { fixed<u8>(kEnd); }

    void begin_list(std::string_view name, Type element, u32 count);
    void begin_list(Type element, u32 count);

    // A payload encoded elsewhere: a subtree out of a Doc, a stored blob.
    void put_raw(std::string_view name, Type type, std::span<const u8> payload);
    void put_raw(std::span<const u8> payload) { b_.append(payload.data(), payload.size()); }

    base::ByteBuf& buf() { return b_; }

  private:
    ALWAYS_INLINE u8* open(usize n) { return b_.reserve(n); }
    static ALWAYS_INLINE u8* header(u8* p, Type type, std::string_view name) {
        DCHECK(name.size() <= kMaxStringBytes);
        *p = type;
        store_be16(p + 1, static_cast<u16>(name.size()));
        std::memcpy(p + 3, name.data(), name.size());
        return p + 3 + name.size();
    }
    template <class T>
    static ALWAYS_INLINE u8* big_endian(u8* p, T v) {
        if constexpr (sizeof(T) == 1) {
            *p = v;
        } else if constexpr (sizeof(T) == 2) {
            store_be16(p, v);
        } else if constexpr (sizeof(T) == 4) {
            store_be32(p, v);
        } else {
            store_be64(p, v);
        }
        return p + sizeof(T);
    }
    template <class T>
    ALWAYS_INLINE void fixed(std::string_view name, Type type, T v) {
        b_.commit(big_endian<T>(header(open(3 + name.size() + sizeof(T)), type, name), v));
    }
    template <class T>
    ALWAYS_INLINE void fixed(T v) {
        b_.commit(big_endian<T>(open(sizeof(T)), v));
    }

    base::ByteBuf& b_;
};

}  // namespace nbt
