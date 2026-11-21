// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto/auth.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdlib>

namespace crypto {

ServerKey::ServerKey() {
    EVP_PKEY* k = EVP_RSA_gen(1024);
    if (!k) std::abort();
    pkey_ = k;
    unsigned char* buf = nullptr;
    const int n = i2d_PUBKEY(k, &buf);
    if (n <= 0) std::abort();
    der_.assign(buf, buf + n);
    OPENSSL_free(buf);
}

ServerKey::~ServerKey() { EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_)); }

int ServerKey::decrypt(std::span<const u8> in, u8* out, usize out_cap) const {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(static_cast<EVP_PKEY*>(pkey_), nullptr);
    if (!ctx) return -1;
    int result = -1;
    size_t len = out_cap;
    if (EVP_PKEY_decrypt_init(ctx) > 0 && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) > 0 &&
        EVP_PKEY_decrypt(ctx, out, &len, in.data(), in.size()) > 0)
        result = static_cast<int>(len);
    EVP_PKEY_CTX_free(ctx);
    return result;
}

std::string server_hash(std::string_view server_id, std::span<const u8> secret, std::span<const u8> public_der) {
    u8 md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, server_id.data(), server_id.size());
    EVP_DigestUpdate(ctx, secret.data(), secret.size());
    EVP_DigestUpdate(ctx, public_der.data(), public_der.size());
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);

    // Java's new BigInteger(bytes).toString(16): two's-complement, minus sign, no leading zeros.
    const bool negative = (md[0] & 0x80) != 0;
    if (negative) {
        unsigned carry = 1;
        for (int i = static_cast<int>(mdlen) - 1; i >= 0; --i) {
            const unsigned v = static_cast<u8>(~md[i]) + carry;
            md[i] = static_cast<u8>(v);
            carry = v >> 8;
        }
    }
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    if (negative) out += '-';
    bool leading = true;
    for (unsigned i = 0; i < mdlen; ++i) {
        for (const u8 nib : {static_cast<u8>(md[i] >> 4), static_cast<u8>(md[i] & 15)}) {
            if (leading && nib == 0) continue;
            leading = false;
            out += kHex[nib];
        }
    }
    if (leading) out += '0';
    return out;
}

Uuid offline_uuid(std::string_view name) {
    std::string in = "OfflinePlayer:";
    in += name;
    u8 md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    EVP_Digest(in.data(), in.size(), md, &mdlen, EVP_md5(), nullptr);
    md[6] = static_cast<u8>((md[6] & 0x0F) | 0x30);  // version 3
    md[8] = static_cast<u8>((md[8] & 0x3F) | 0x80);  // IETF variant
    return {load_be64(md), load_be64(md + 8)};
}

void random_bytes(u8* out, usize n) {
    if (RAND_bytes(out, static_cast<int>(n)) != 1) std::abort();
}

Uuid random_uuid() {
    u8 b[16];
    random_bytes(b, 16);
    b[6] = static_cast<u8>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<u8>((b[8] & 0x3F) | 0x80);
    return {load_be64(b), load_be64(b + 8)};
}

std::string uuid_to_string(Uuid u) {
    static const char kHex[] = "0123456789abcdef";
    u8 b[16];
    store_be64(b, u.hi);
    store_be64(b + 8, u.lo);
    std::string s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 15];
    }
    return s;
}

bool uuid_from_string(std::string_view s, Uuid& out) {
    u8 b[16];
    int nibbles = 0;
    for (char c : s) {
        if (c == '-') continue;
        int v;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            return false;
        if (nibbles >= 32) return false;
        if (nibbles & 1)
            b[nibbles >> 1] = static_cast<u8>(b[nibbles >> 1] | v);
        else
            b[nibbles >> 1] = static_cast<u8>(v << 4);
        ++nibbles;
    }
    if (nibbles != 32) return false;
    out = {load_be64(b), load_be64(b + 8)};
    return true;
}

}  // namespace crypto
