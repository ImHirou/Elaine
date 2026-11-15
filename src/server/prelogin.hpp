// Handshake / status / login / configuration, i.e. everything a connection does before it
// becomes a player. Runs on reactor threads; one shared instance, immutable after construction
// (per-connection state hangs off net::Conn::pre).
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "crypto/auth.hpp"
#include "net/reactor.hpp"
#include "proto/wire.hpp"
#include "server/session_auth.hpp"
#include "server/settings.hpp"

namespace server {

// What the tick thread learns about a new player (payload of the Join record).
struct JoinInfo {
    Uuid uuid;
    std::string name;
    std::vector<Property> props;
    u8 view_distance = 10;
    u8 skin_parts = 0;  // displayed model parts (cape, jacket, ...), a bit set
    u8 main_hand = 1;   // HumanoidArm id: 0 left, 1 right
    std::string remote;
};
void encode_join(base::ByteBuf& out, const JoinInfo& j);
bool decode_join(const u8* p, usize n, JoinInfo& out);

class Prelogin final : public net::PreSink {
  public:
    Prelogin(const Settings& s, const std::atomic<u32>& online);
    ~Prelogin();

    void on_open(net::Reactor& r, net::Conn& c) override;
    void on_frame(net::Reactor& r, net::Conn& c, std::span<const u8> body) override;
    void on_close(net::Reactor& r, net::Conn& c) override;

  private:
    struct Pre;
    void handshake(net::Reactor& r, net::Conn& c, u32 id, proto::Reader& rd);
    void status(net::Reactor& r, net::Conn& c, u32 id, proto::Reader& rd);
    void login(net::Reactor& r, net::Conn& c, u32 id, proto::Reader& rd);
    void config(net::Reactor& r, net::Conn& c, u32 id, proto::Reader& rd);
    void finish_login(net::Reactor& r, net::Conn& c);
    void login_kick(net::Reactor& r, net::Conn& c, std::string_view text);
    void config_kick(net::Reactor& r, net::Conn& c, std::string_view text);

    const Settings& s_;
    const std::atomic<u32>& online_;
    Uuid session_id_;
    std::unique_ptr<crypto::ServerKey> key_;  // online mode only
    std::unique_ptr<SessionAuth> auth_;       // online mode only
    base::ByteBuf config_burst_;              // framed registry_data + tags for s_.compression_threshold
};

}  // namespace server
