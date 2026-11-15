#include <pthread.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "base/log.hpp"
#include "net/reactor.hpp"
#include "proto/gen/current.hpp"
#include "server/console.hpp"
#include "server/game.hpp"
#include "server/prelogin.hpp"
#include "server/settings.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }
}  // namespace

int main(int argc, char** argv) {
    server::Settings s;
    std::string config_path = "server.properties", err;
    if (!server::apply_args(argc, argv, s, config_path, err)) {
        std::fprintf(stderr, "error: %s\nusage: %s [--config=server.properties] [--key=value ...]\n", err.c_str(), argv[0]);
        return 2;
    }
    if (s.debug) base::set_log_level(base::LogLevel::Debug);

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    // The console reads stdin. A background job that tries is stopped by the terminal unless
    // it says it does not mind: then the read fails and the server runs without a console.
    std::signal(SIGTTIN, SIG_IGN);

    std::atomic<u32> online{0};
    server::Prelogin prelogin(s, online);

    std::vector<std::unique_ptr<net::Reactor>> reactors;
    for (u32 i = 0; i < s.net_threads; ++i) {
        net::ReactorConfig rc;
        rc.bind_ip = s.bind_ip;
        rc.port = s.port;
        rc.max_conns = s.max_players + 256;  // head-room for status pings and logins in progress
        rc.prelogin_timeout_ns = static_cast<u64>(s.login_timeout) * 1'000'000'000ull;
        reactors.push_back(std::make_unique<net::Reactor>(rc, prelogin, i));
        if (!reactors.back()->listen(err)) {
            LOG_ERROR("%s", err.c_str());
            return 1;
        }
    }
    std::vector<std::thread> threads;
    for (auto& r : reactors) {
        threads.emplace_back([&r] {
            char name[16];
            std::snprintf(name, sizeof name, "net-%u", r->index());
            pthread_setname_np(pthread_self(), name);
            r->run();
        });
    }
    LOG_INFO("Minecraft %.*s (protocol %d) listening on %s:%u - %s mode, compression %d, %u net thread(s)",
             static_cast<int>(proto::gen::kVersionName.size()), proto::gen::kVersionName.data(), proto::gen::kProtocolVersion,
             s.bind_ip.c_str(), s.port, s.online_mode ? "online" : "OFFLINE", s.compression_threshold, s.net_threads);

    pthread_setname_np(pthread_self(), "tick");
    {
        server::Console console;
        console.start();
        server::Game game(s, reactors, online, &console);
        game.run(g_stop);
    }
    LOG_INFO("stopping");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let "Server closed" reach the clients
    for (auto& r : reactors) r->stop();
    for (auto& t : threads) t.join();
    return 0;
}
