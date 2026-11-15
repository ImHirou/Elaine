// Login-time crypto (cold path, OpenSSL): the server RSA key, shared-secret decryption, the
// session-server hash, offline-mode UUIDs and random bytes.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/types.hpp"

namespace crypto {

class ServerKey {
  public:
    ServerKey();  // generates a fresh RSA-1024 key pair (what the protocol expects)
    ~ServerKey();
    ServerKey(const ServerKey&) = delete;
    ServerKey& operator=(const ServerKey&) = delete;

    std::span<const u8> public_der() const { return der_; }  // X.509 SubjectPublicKeyInfo

    // PKCS#1 v1.5 decrypt. Returns plaintext length, or -1. Safe to call from several threads.
    int decrypt(std::span<const u8> in, u8* out, usize out_cap) const;

  private:
    void* pkey_;  // EVP_PKEY*
    std::vector<u8> der_;
};

// Hex digest the client and session server agree on:
// SHA-1(server_id || shared_secret || public_key), printed as a signed big-endian integer.
std::string server_hash(std::string_view server_id, std::span<const u8> secret, std::span<const u8> public_der);

// UUID.nameUUIDFromBytes("OfflinePlayer:" + name): version-3 (MD5) UUID used in offline mode.
Uuid offline_uuid(std::string_view name);

void random_bytes(u8* out, usize n);
Uuid random_uuid();  // version 4

// "069a79f4-44e9-4726-a5be-fca90e38aaf5"
std::string uuid_to_string(Uuid u);
// Accepts dashed or undashed hex. Returns false if malformed.
bool uuid_from_string(std::string_view s, Uuid& out);

}  // namespace crypto
