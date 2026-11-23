// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/settings.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string_view>

namespace server {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

template <class T>
bool parse_num(std::string_view v, T& out, long long lo, long long hi) {
    long long n = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc() || p != v.data() + v.size() || n < lo || n > hi) return false;
    out = static_cast<T>(n);
    return true;
}

bool parse_float(std::string_view v, float& out, float lo, float hi) {
    float f = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), f);
    if (ec != std::errc() || p != v.data() + v.size() || !(f >= lo && f <= hi)) return false;
    out = f;
    return true;
}

bool parse_bool(std::string_view v, bool& out) {
    if (v == "true" || v == "1")
        out = true;
    else if (v == "false" || v == "0")
        out = false;
    else
        return false;
    return true;
}

bool set_key(Settings& s, std::string_view k, std::string_view v, std::string& err) {
    bool ok = true;
    if (k == "server-ip")
        s.bind_ip = v.empty() ? "0.0.0.0" : std::string(v);
    else if (k == "server-port")
        ok = parse_num(v, s.port, 1, 65535);
    else if (k == "motd")
        s.motd = std::string(v);
    else if (k == "max-players")
        ok = parse_num(v, s.max_players, 1, 100000);
    else if (k == "online-mode")
        ok = parse_bool(v, s.online_mode);
    else if (k == "network-compression-threshold")
        ok = parse_num(v, s.compression_threshold, -1, 1 << 20);
    else if (k == "view-distance")
        ok = parse_num(v, s.view_distance, 2, 32);
    else if (k == "simulation-distance")
        ok = parse_num(v, s.simulation_distance, 2, 32);
    else if (k == "entity-broadcast-range-percentage") {
        ok = parse_num(v, s.entity_broadcast_range, 0, 1 << 20);
        s.entity_broadcast_range = std::clamp<u32>(s.entity_broadcast_range, 10, 1000);  // as DedicatedServerProperties does
    } else if (k == "net-threads")
        ok = parse_num(v, s.net_threads, 1, 64);
    else if (k == "login-timeout")
        ok = parse_num(v, s.login_timeout, 1, 3600);
    else if (k == "session-server")
        s.session_server = std::string(v);
    else if (k == "server-brand")
        s.brand = std::string(v);
    else if (k == "debug")
        ok = parse_bool(v, s.debug);
    else if (k == "op-permission-level")
        ok = parse_num(v, s.op_permission_level, 0, 4);
    else if (k == "command-spam-threshold-seconds")
        ok = parse_num(v, s.command_spam_threshold, 0, 1000000);
    else if (k == "chat-spam-threshold-seconds")
        ok = parse_num(v, s.chat_spam_threshold, 0, 1000000);
    else if (k == "enforce-secure-profile")
        ok = parse_bool(v, s.enforce_secure_profile);
    else if (k == "ops-file")
        s.ops_file = std::string(v);
    else if (k == "level-name")
        s.level_name = std::string(v);
    else if (k == "sync-chunk-writes")
        ok = parse_bool(v, s.sync_chunk_writes);
    else if (k == "worker-threads")
        ok = parse_num(v, s.worker_threads, 0, 64);
    else if (k == "chunk-unload-seconds")
        ok = parse_num(v, s.chunk_unload_seconds, 1, 86400);
    else if (k == "autosave-seconds")
        ok = parse_num(v, s.autosave_seconds, 1, 86400);
    else if (k == "max-chained-neighbor-updates")
        ok = parse_num(v, s.max_chained_neighbor_updates, -(1 << 30), 1 << 30);
    else if (k == "random-tick-speed")
        ok = parse_num(v, s.random_tick_speed, 0, 4096);
    else if (k == "advance-time")
        ok = parse_bool(v, s.advance_time);
    else if (k == "water-source-conversion")
        ok = parse_bool(v, s.water_source_conversion);
    else if (k == "lava-source-conversion")
        ok = parse_bool(v, s.lava_source_conversion);
    else if (k == "tick-rate")
        ok = parse_float(v, s.tick_rate, 1.0f, 10000.0f);  // (TickCommand: floatArg(1, 10000))
    else
        return true;  // unknown keys (vanilla has many we do not implement yet) are ignored
    if (!ok) err = "invalid value for " + std::string(k) + ": '" + std::string(v) + "'";
    return ok;
}

}  // namespace

bool load_properties(const std::string& path, Settings& s, std::string& err) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return true;
    char line[4096];
    bool ok = true;
    while (ok && std::fgets(line, sizeof line, f)) {
        std::string_view l = trim(line);
        if (l.empty() || l.front() == '#' || l.front() == '!') continue;
        const auto eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        ok = set_key(s, trim(l.substr(0, eq)), trim(l.substr(eq + 1)), err);
    }
    std::fclose(f);
    return ok;
}

bool apply_args(int argc, char** argv, Settings& s, std::string& config_path, std::string& err) {
    // First pass: --config must be known before the file is read; flags then override the file.
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.starts_with("--config=")) config_path = std::string(a.substr(9));
    }
    if (!load_properties(config_path, s, err)) return false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (!a.starts_with("--")) {
            err = "unexpected argument: " + std::string(a);
            return false;
        }
        a.remove_prefix(2);
        if (a.starts_with("config=")) continue;
        const auto eq = a.find('=');
        if (eq == std::string_view::npos) {
            err = "expected --key=value, got --" + std::string(a);
            return false;
        }
        if (!set_key(s, a.substr(0, eq), a.substr(eq + 1), err)) return false;
    }
    return true;
}

}  // namespace server
