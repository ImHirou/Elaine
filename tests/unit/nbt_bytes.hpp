// SPDX-License-Identifier: GPL-3.0-or-later
// NBT put together by hand, for tests: each function returns the bytes the tag layout of the
// 26.3 jar (NbtIo, the TagTypes, ListTag) prescribes. Independent of nbt::Writer on purpose.
#pragma once

#include <initializer_list>
#include <string_view>
#include <vector>

#include "base/types.hpp"

namespace nbt_bytes {

using Bytes = std::vector<u8>;

inline Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
inline Bytes be16(u32 v) { return {static_cast<u8>(v >> 8), static_cast<u8>(v)}; }
inline Bytes be32(u32 v) {
    return {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
}
inline Bytes be64(u64 v) { return be32(static_cast<u32>(v >> 32)) + be32(static_cast<u32>(v)); }
inline Bytes raw(std::string_view s) { return {s.begin(), s.end()}; }
// The payload of a string, or a name: a length and the bytes.
inline Bytes str(std::string_view s) { return be16(static_cast<u32>(s.size())) + raw(s); }

// An entry of a compound: type, name, payload.
inline Bytes entry(u8 type, std::string_view name, const Bytes& payload) { return Bytes{type} + str(name) + payload; }
// The payload of a compound: its entries and TAG_End.
inline Bytes compound(std::initializer_list<Bytes> entries) {
    Bytes out;
    for (const Bytes& e : entries) out = out + e;
    return out + Bytes{0};
}
// The payload of a list: element type, count, the elements' payloads.
inline Bytes list(u8 type, std::initializer_list<Bytes> elements) {
    Bytes out = Bytes{type} + be32(static_cast<u32>(elements.size()));
    for (const Bytes& e : elements) out = out + e;
    return out;
}
// A tag without a name: the network form.
inline Bytes root(u8 type, const Bytes& payload) { return Bytes{type} + payload; }

// Entries by type, for compounds that read like the SNBT they stand for.
inline Bytes b(std::string_view name, u8 v) { return entry(1, name, {v}); }
inline Bytes i(std::string_view name, u32 v) { return entry(3, name, be32(v)); }
inline Bytes s(std::string_view name, std::string_view v) { return entry(8, name, str(v)); }
inline Bytes c(std::string_view name, std::initializer_list<Bytes> entries) { return entry(10, name, compound(entries)); }
inline Bytes l(std::string_view name, u8 type, std::initializer_list<Bytes> elements) {
    return entry(9, name, list(type, elements));
}

}  // namespace nbt_bytes
