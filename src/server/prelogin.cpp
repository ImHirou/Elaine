// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/prelogin.hpp"

#include "base/log.hpp"
#include "proto/packets.hpp"
#include "proto/static_data.hpp"

namespace server {

using namespace proto;
using net::Conn;
using net::Phase;
using net::Reactor;

struct Prelogin::Pre {
    enum class Stage { Hello, Key, Auth, Ack, KnownPacks, Finish } stage = Stage::Hello;
    std::string name;
    Uuid uuid;
    std::vector<Property> props;
    u8 challenge[4] = {};
    u8 view_distance = 10;
    u8 skin_parts = 0;  // ClientInformation.createDefault(): what a client that never sends its options gets
    u8 main_hand = 1;
};

void encode_join(base::ByteBuf& out, const JoinInfo& j) {
    usize bound = 16 + str_bound(j.name) + 16 + str_bound(j.remote);
    for (const auto& p : j.props) bound += str_bound(p.name) + str_bound(p.value) + str_bound(p.signature);
    u8* p = out.reserve(bound);
    p = put_uuid(p, j.uuid);
    p = put_str(p, j.name);
    p = put_varint(p, static_cast<u32>(j.props.size()));
    for (const auto& pr : j.props) {
        p = put_str(p, pr.name);
        p = put_str(p, pr.value);
        p = put_str(p, pr.signature);
    }
    p = put_u8(p, j.view_distance);
    p = put_u8(p, j.skin_parts);
    p = put_u8(p, j.main_hand);
    p = put_str(p, j.remote);
    out.commit(p);
}

bool decode_join(const u8* data, usize n, JoinInfo& out) {
    Reader r(data, n);
    out.uuid = r.uuid();
    out.name = std::string(r.str(64));
    const u32 np = r.varint();
    for (u32 i = 0; i < np && r.ok; ++i) {
        Property p;
        p.name = std::string(r.str(1 << 16));
        p.value = std::string(r.str(1 << 16));
        p.signature = std::string(r.str(1 << 16));
        out.props.push_back(std::move(p));
    }
    out.view_distance = r.u8v();
    out.skin_parts = r.u8v();
    out.main_hand = r.u8v();
    out.remote = std::string(r.str(128));
    return r.ok;
}

Prelogin::Prelogin(const Settings& s, const std::atomic<u32>& online)
    : s_(s), online_(online), session_id_(crypto::random_uuid()) {
    if (s_.online_mode) {
        key_ = std::make_unique<crypto::ServerKey>();
        auth_ = std::make_unique<SessionAuth>(s_.session_server);
    }
    build_config_burst(config_burst_, s_.compression_threshold);
}

Prelogin::~Prelogin() = default;

void Prelogin::on_open(Reactor&, Conn& c) { c.pre = new Pre(); }

void Prelogin::on_close(Reactor&, Conn& c) {
    delete static_cast<Pre*>(c.pre);
    c.pre = nullptr;
}

void Prelogin::on_frame(Reactor& r, Conn& c, std::span<const u8> body) {
    Reader rd(body);
    const u32 id = rd.varint();
    if (!rd.ok) {
        r.close(c);
        return;
    }
    switch (c.phase) {
        case Phase::Handshake: handshake(r, c, id, rd); break;
        case Phase::Status: status(r, c, id, rd); break;
        case Phase::Login: login(r, c, id, rd); break;
        case Phase::Config: config(r, c, id, rd); break;
        case Phase::Play: break;  // never routed here
    }
}

void Prelogin::login_kick(Reactor& r, Conn& c, std::string_view text) {
    std::string json = "{\"text\":\"";
    json_escape(json, text);
    json += "\"}";
    r.send(c, cb::LoginDisconnect{json});
    r.close_after_flush(c);
}

void Prelogin::config_kick(Reactor& r, Conn& c, std::string_view text) {
    r.send(c, cb::ConfigDisconnect{text});
    r.close_after_flush(c);
}

// ---------------------------------------------------------------------------- handshake

void Prelogin::handshake(Reactor& r, Conn& c, u32 id, Reader& rd) {
    const auto h = sb::Handshake::decode(rd);
    if (id != ids::handshake::sb::kIntention || !rd.ok) {
        r.close(c);
        return;
    }
    if (h.intent == 1) {
        r.set_phase(c, Phase::Status);
        return;
    }
    if (h.intent != 2 && h.intent != 3) {
        r.close(c);
        return;
    }
    r.set_phase(c, Phase::Login);
    if (h.protocol != static_cast<u32>(gen::kProtocolVersion)) {
        std::string msg = h.protocol < static_cast<u32>(gen::kProtocolVersion) ? "Outdated client! Please use "
                                                                               : "Outdated server! I'm still on ";
        msg += gen::kVersionName;
        login_kick(r, c, msg);
    } else if (h.intent == 3) {
        login_kick(r, c, "This server does not accept transfers.");
    }
}

// ---------------------------------------------------------------------------- status

void Prelogin::status(Reactor& r, Conn& c, u32 id, Reader& rd) {
    if (id == ids::status::sb::kStatusRequest) {
        std::string json = "{\"version\":{\"name\":\"";
        json += gen::kVersionName;
        json += "\",\"protocol\":" + std::to_string(gen::kProtocolVersion);
        json += "},\"players\":{\"max\":" + std::to_string(s_.max_players);
        json += ",\"online\":" + std::to_string(online_.load(std::memory_order_relaxed));
        json += "},\"description\":{\"text\":\"";
        json_escape(json, s_.motd);
        json += "\"},\"enforcesSecureChat\":false}";
        r.send(c, cb::StatusResponse{json});
    } else if (id == ids::status::sb::kPingRequest) {
        const u64 t = rd.be64();
        if (!rd.ok) {
            r.close(c);
            return;
        }
        r.send(c, cb::StatusPong{t});
        r.close_after_flush(c);
    } else {
        r.close(c);
    }
}

// ---------------------------------------------------------------------------- login

void Prelogin::login(Reactor& r, Conn& c, u32 id, Reader& rd) {
    Pre& pre = *static_cast<Pre*>(c.pre);
    using Stage = Pre::Stage;

    if (id == ids::login::sb::kHello) {
        const auto hello = sb::LoginStart::decode(rd);
        bool valid = rd.ok && pre.stage == Stage::Hello && !hello.name.empty() && hello.name.size() <= 16;
        for (const char ch : hello.name) valid = valid && ch > 32 && ch < 127;
        if (!valid) {
            r.close(c);
            return;
        }
        pre.name = std::string(hello.name);
        if (s_.online_mode) {
            crypto::random_bytes(pre.challenge, sizeof pre.challenge);
            r.send(c, cb::EncryptionRequest{"", key_->public_der(), pre.challenge, true});
            pre.stage = Stage::Key;
        } else {
            pre.uuid = crypto::offline_uuid(pre.name);
            finish_login(r, c);
        }
    } else if (id == ids::login::sb::kKey) {
        const auto key = sb::EncryptionResponse::decode(rd);
        if (!rd.ok || pre.stage != Stage::Key || !s_.online_mode) {
            r.close(c);
            return;
        }
        u8 buf[256];
        int n = key_->decrypt(key.challenge, buf, sizeof buf);
        if (n != sizeof pre.challenge || std::memcmp(buf, pre.challenge, sizeof pre.challenge) != 0) {
            r.close(c);
            return;
        }
        u8 secret[256];
        n = key_->decrypt(key.secret, secret, sizeof secret);
        if (n != 16) {
            r.close(c);
            return;
        }
        r.enable_encryption(c, secret);
        pre.stage = Stage::Auth;
        const std::string hash = crypto::server_hash("", {secret, 16}, key_->public_der());
        Reactor* rp = &r;
        const u32 slot = c.slot, gen = c.gen;
        auth_->verify(pre.name, hash, [this, rp, slot, gen](AuthResult&& res) {
            rp->run_on_loop([this, rp, slot, gen, res = std::move(res)]() {
                Conn* conn = rp->lookup(slot, gen);
                if (!conn || !conn->pre) return;  // gone while we were asking
                Pre& p = *static_cast<Pre*>(conn->pre);
                if (p.stage != Pre::Stage::Auth) return;
                if (!res.ok) {
                    LOG_INFO("%s: login of '%s' refused: %s", conn->remote.c_str(), p.name.c_str(), res.error.c_str());
                    login_kick(*rp, *conn, "Failed to verify username!");
                    return;
                }
                p.uuid = res.uuid;
                p.name = res.name;
                p.props = res.props;
                finish_login(*rp, *conn);
            });
        });
    } else if (id == ids::login::sb::kLoginAcknowledged) {
        if (pre.stage != Stage::Ack) {
            r.close(c);
            return;
        }
        r.set_phase(c, Phase::Config);
        r.send(c, cb::ConfigBrand{s_.brand});
        r.send(c, cb::EnabledFeatures{});
        r.send(c, cb::SelectKnownPacks{});
        pre.stage = Stage::KnownPacks;
    }
    // custom_query_answer / cookie_response: nothing asks for them yet
}

void Prelogin::finish_login(Reactor& r, Conn& c) {
    Pre& pre = *static_cast<Pre*>(c.pre);
    if (online_.load(std::memory_order_relaxed) >= s_.max_players) {
        login_kick(r, c, "The server is full!");
        return;
    }
    if (s_.compression_threshold >= 0) {
        r.send(c, cb::LoginCompression{s_.compression_threshold});  // itself still uncompressed
        r.set_compression(c, s_.compression_threshold);
    }
    std::vector<ProfileProperty> props;
    props.reserve(pre.props.size());
    for (const auto& p : pre.props) props.push_back({p.name, p.value, p.signature});
    r.send(c, cb::LoginFinished{pre.uuid, pre.name, props, session_id_});
    pre.stage = Pre::Stage::Ack;
}

// ---------------------------------------------------------------------------- configuration

void Prelogin::config(Reactor& r, Conn& c, u32 id, Reader& rd) {
    Pre& pre = *static_cast<Pre*>(c.pre);
    using Stage = Pre::Stage;

    if (id == ids::configuration::sb::kClientInformation) {
        const auto info = sb::ClientInformation::decode(rd);
        if (rd.ok && info.view_distance >= 2) pre.view_distance = static_cast<u8>(info.view_distance);
        if (rd.ok) {
            pre.skin_parts = info.skin_parts;
            pre.main_hand = info.main_hand == 1 ? 1 : 0;  // HumanoidArm.BY_ID: anything unknown is LEFT
        }
    } else if (id == ids::configuration::sb::kSelectKnownPacks) {
        if (pre.stage != Stage::KnownPacks) {
            r.close(c);
            return;
        }
        // We send registry entries by name only, which is valid only if the client has the
        // vanilla data of exactly our version.
        bool has_core = false;
        const u32 n = rd.varint();
        for (u32 i = 0; i < n && i < 64 && rd.ok; ++i) {
            const auto ns = rd.str(32767), pid = rd.str(32767), ver = rd.str(32767);
            if (ns == "minecraft" && pid == "core" && ver == gen::kVersionName) has_core = true;
        }
        if (!rd.ok) {
            r.close(c);
            return;
        }
        if (!has_core) {
            config_kick(r, c, "This server needs the vanilla data pack of your client (minecraft:core).");
            return;
        }
        r.send_raw_frames(c, {config_burst_.data, config_burst_.len});
        r.send(c, cb::FinishConfiguration{});
        pre.stage = Stage::Finish;
    } else if (id == ids::configuration::sb::kFinishConfiguration) {
        if (pre.stage != Stage::Finish) {
            r.close(c);
            return;
        }
        JoinInfo j;
        j.uuid = pre.uuid;
        j.name = std::move(pre.name);
        j.props = std::move(pre.props);
        j.view_distance = pre.view_distance;
        j.skin_parts = pre.skin_parts;
        j.main_hand = pre.main_hand;
        j.remote = c.remote;
        base::ByteBuf blob(512);
        encode_join(blob, j);
        delete &pre;
        c.pre = nullptr;
        if (!r.promote_to_play(c, {blob.data, blob.len})) {
            LOG_WARN("%s: could not hand '%s' to the game thread (queue full)", c.remote.c_str(), j.name.c_str());
            r.close(c);
        }
    }
    // custom_payload (client brand), keep_alive, pong, resource_pack, cookie_response: ignored for now
}

}  // namespace server
