// Server settings: server.properties-compatible keys plus a few of our own, overridable on the
// command line as --key=value (e.g. --server-port=25570 --online-mode=false).
#pragma once

#include <string>

#include "base/types.hpp"

namespace server {

struct Settings {
    std::string bind_ip = "0.0.0.0";  // server-ip
    u16 port = 25565;                 // server-port
    std::string motd = "A Minecraft Server";
    u32 max_players = 500;             // max-players
    bool online_mode = true;           // online-mode
    i32 compression_threshold = 256;   // network-compression-threshold (-1 = off)
    u32 view_distance = 10;            // view-distance
    u32 simulation_distance = 10;      // simulation-distance
    u32 entity_broadcast_range = 100;  // entity-broadcast-range-percentage: scales how far entities are shown
    u32 net_threads = 1;               // net-threads: io_uring reactors (1 is plenty for ~1000 players)
    u32 login_timeout = 30;            // login-timeout: seconds a connection may take to become a player
    std::string session_server = "https://sessionserver.mojang.com";  // session-server
    std::string brand = "vanilla";                                    // server-brand: shown on the client's F3 screen
    bool debug = false;                                               // debug: verbose logging
    u32 op_permission_level = 4;                                      // op-permission-level: what /op gives
    u32 command_spam_threshold = 10;    // command-spam-threshold-seconds: commands in a row that get a player thrown out
    std::string ops_file = "ops.json";  // ops-file: the operators (vanilla's ops.json)
    // level-name: the world's directory (server/world_info.hpp). Empty: the world is not kept -
    // nothing is written and no chunk leaves memory.
    std::string level_name = "world";
    bool sync_chunk_writes = true;  // sync-chunk-writes: fdatasync around every write of a region (world/region.hpp)
    u32 worker_threads = 0;         // worker-threads: threads that read, write and generate chunks; 0 = by the machine
    u32 chunk_unload_seconds = 10;  // chunk-unload-seconds: how long a chunk stays loaded once nobody is near
    u32 autosave_seconds = 300;     // autosave-seconds: how long a chunk that changed may wait to be written
    // max-chained-neighbor-updates: how many neighbour updates one block change may set off
    // before the rest are skipped (CollectingNeighborUpdater); negative: any number.
    i32 max_chained_neighbor_updates = 1000000;
    u32 random_tick_speed = 3;  // random-tick-speed: the game rule randomTickSpeed, until there are game rules
};

// Missing file is not an error (defaults apply). Returns false + err on a malformed value.
bool load_properties(const std::string& path, Settings& s, std::string& err);
bool apply_args(int argc, char** argv, Settings& s, std::string& config_path, std::string& err);

}  // namespace server
