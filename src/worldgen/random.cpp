// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/random.hpp"

#include <openssl/evp.h>

#include <charconv>

namespace worldgen {

Seed128 seed_from_hash_of(std::string_view name) {
    // Hashing.md5().hashString(name, UTF_8).asBytes(), then Longs.fromBytes of each half.
    u8 md[EVP_MAX_MD_SIZE] = {};
    unsigned int len = 0;
    EVP_Digest(name.data(), name.size(), md, &len, EVP_md5(), nullptr);
    return {load_be64(md), load_be64(md + 8)};
}

i32 java_string_hash(std::string_view utf8) {
    // s[0]*31^(n-1) + ... + s[n-1] over UTF-16 code units: a character beyond the BMP is two.
    u32 h = 0;
    const auto unit = [&](u32 c) { h = h * 31u + c; };
    for (usize i = 0; i < utf8.size();) {
        const u8 b = static_cast<u8>(utf8[i]);
        u32 c = b, more = 0;
        if (b >= 0xF0)
            c = b & 0x07, more = 3;
        else if (b >= 0xE0)
            c = b & 0x0F, more = 2;
        else if (b >= 0xC0)
            c = b & 0x1F, more = 1;
        ++i;
        for (; more && i < utf8.size(); --more, ++i) c = c << 6 | (static_cast<u8>(utf8[i]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            unit(0xD800 + (c >> 10));
            unit(0xDC00 + (c & 0x3FF));
        } else {
            unit(c);
        }
    }
    return static_cast<i32>(h);
}

i64 obfuscate_seed(i64 seed) {
    // Hashing.sha256().hashLong(seed).asLong(): Guava hashes the long's bytes low byte first and
    // reads the hash's first eight bytes the same way.
    u8 in[8], md[EVP_MAX_MD_SIZE] = {};
    store_le<i64>(in, seed);
    unsigned int len = 0;
    EVP_Digest(in, sizeof in, md, &len, EVP_sha256(), nullptr);
    return load_le<i64>(md);
}

bool parse_seed(std::string_view text, i64& out) {
    // String.trim(): everything up to U+0020 at either end.
    while (!text.empty() && static_cast<u8>(text.front()) <= 0x20) text.remove_prefix(1);
    while (!text.empty() && static_cast<u8>(text.back()) <= 0x20) text.remove_suffix(1);
    if (text.empty()) return false;
    // Long.parseLong: an optional sign, then decimal digits, the whole string, in range.
    std::string_view digits = text;
    if (digits.front() == '+') digits.remove_prefix(1);  // (from_chars takes '-' and not '+')
    i64 n = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    const bool number =
        ec == std::errc() && end == digits.data() + digits.size() && !(text.front() == '+' && digits.front() == '-');
    out = number ? n : static_cast<i64>(java_string_hash(text));
    return true;
}

}  // namespace worldgen
