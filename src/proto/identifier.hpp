// Identifier ("namespace:path"): the name of everything in a registry, an asset, a font.
#pragma once

#include <algorithm>
#include <string_view>

#include "base/types.hpp"

namespace proto {

struct Identifier {
    std::string_view ns, path;
    usize size() const { return ns.size() + 1 + path.size(); }  // of "ns:path", as Identifier.toString writes it
    u8* put(u8* p) const {
        std::memcpy(p, ns.data(), ns.size());
        p[ns.size()] = ':';
        std::memcpy(p + ns.size() + 1, path.data(), path.size());
        return p + size();
    }
};

// Identifier.parse (and Identifier.read, its DataResult twin): everything before the first ':'
// is the namespace, unless there is none or it is empty - then it is "minecraft". False where
// vanilla throws ResourceLocationException. The parts are views into `s` (or the literal).
inline bool parse_identifier(std::string_view s, Identifier& out) {
    const auto namespace_char = [](char c) {
        return c == '_' || c == '-' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
    };
    const auto path_char = [&](char c) { return namespace_char(c) || c == '/'; };
    const usize colon = s.find(':');
    out.ns = colon == std::string_view::npos || colon == 0 ? std::string_view("minecraft") : s.substr(0, colon);
    out.path = colon == std::string_view::npos ? s : s.substr(colon + 1);
    return out.ns != ".." && std::all_of(out.ns.begin(), out.ns.end(), namespace_char) &&
           std::all_of(out.path.begin(), out.path.end(), path_char);
}

}  // namespace proto
