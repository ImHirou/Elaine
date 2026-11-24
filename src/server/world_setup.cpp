// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <unistd.h>
#include <yyjson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>

#include "base/log.hpp"
#include "server/import.hpp"
#include "server/world_info.hpp"
#include "world/block.hpp"
#include "worldgen/generator.hpp"
#include "worldgen/random.hpp"
#include "worldgen/terrain.hpp"

namespace server {

namespace {

constexpr i64 kFormat = 1;  // of world.json and of the chunk bytes (world/chunk_io.hpp)

// WorldOptions.randomSeed(): any 64 bits.
i64 random_seed() {
    std::random_device device;
    return static_cast<i64>(static_cast<u64>(device()) << 32 | device());
}

// What generates a world that is made new: `level-type`, a world preset by name
// (DedicatedServerProperties.WorldDimensionData.create).
bool new_generator(const Settings& s, WorldInfo& out, std::string& err) {
    const std::string type = s.level_type.find(':') == std::string::npos ? "minecraft:" + s.level_type : s.level_type;
    if (type == "minecraft:flat") return true;  // (as WorldInfo begins)
    for (const char* missing : {"minecraft:large_biomes", "minecraft:amplified", "minecraft:single_biome_surface",
                                "minecraft:debug_all_block_states"}) {
        if (type != missing) continue;
        err = "level-type " + s.level_type +
              ": this server has no generator for it yet (it has minecraft:normal and minecraft:flat)";
        return false;
    }
    // Vanilla takes a name it does not know for the default, and says so.
    if (type != "minecraft:normal")
        LOG_WARN("Failed to parse level-type %s, defaulting to minecraft:normal", s.level_type.c_str());
    out.flat = false;
    out.noise = true;
    out.sea_level = worldgen::settings::kSeaLevel;
    return true;
}

// `seeded`: whether the file names the world's seed (one written before worlds had seeds does not).
bool read_meta(const std::string& path, WorldInfo& out, bool& seeded, std::string& err) {
    yyjson_doc* doc = yyjson_read_file(path.c_str(), 0, nullptr, nullptr);
    if (!doc) {
        err = path + " cannot be read";
        return false;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    const i64 format = yyjson_get_sint(yyjson_obj_get(root, "format"));
    const i64 version = yyjson_get_sint(yyjson_obj_get(root, "data_version"));
    bool ok = true;
    if (format != kFormat) {
        err = path + ": a world of format " + std::to_string(format) + ", this server reads " + std::to_string(kFormat);
        ok = false;
    } else if (version != proto::gen::kWorldVersion) {
        // Chunks hold this version's ids of block states, biomes and items.
        err = path + ": the world was saved by data version " + std::to_string(version) + ", this server is " +
              std::to_string(proto::gen::kWorldVersion) + " (" + std::string(proto::gen::kVersionName) +
              ") and cannot convert it yet";
        ok = false;
    }
    if (ok) {
        yyjson_val* seed = yyjson_obj_get(root, "seed");
        seeded = yyjson_is_int(seed);
        if (seeded) out.seed = yyjson_get_sint(seed);
        yyjson_val* gen = yyjson_obj_get(root, "generator");
        const char* type = yyjson_get_str(yyjson_obj_get(gen, "type"));
        out.flat = !type || std::string_view(type) == "flat";
        out.noise = type && std::string_view(type) == "noise";
        out.sea_level = static_cast<i32>(yyjson_get_sint(yyjson_obj_get(gen, "sea_level")));
        if (const char* biome = yyjson_get_str(yyjson_obj_get(gen, "biome"))) {
            if (const u32 id = world::find_biome(biome); id != world::kNoId) out.biome = static_cast<u16>(id);
        }
        if (out.flat) {
            out.layers.clear();
            yyjson_val* layer;
            yyjson_arr_iter it;
            if (yyjson_arr_iter_init(yyjson_obj_get(gen, "layers"), &it)) {
                while ((layer = yyjson_arr_iter_next(&it))) {
                    const char* block = yyjson_get_str(yyjson_obj_get(layer, "block"));
                    const u32 id = block ? world::find_block(block) : world::kNoId;
                    const i64 height = yyjson_get_sint(yyjson_obj_get(layer, "height"));
                    if (id == world::kNoId || height < 0 || height > 4064) {
                        err = path + ": a layer of the generator names no block";
                        ok = false;
                        break;
                    }
                    out.layers.push_back({world::block_info(id).default_state, static_cast<u32>(height)});
                }
            }
        }
        yyjson_val* spawn = yyjson_obj_get(root, "spawn");
        if (yyjson_arr_size(spawn) == 3) {
            for (usize i = 0; i < 3; ++i) out.spawn[i] = static_cast<i32>(yyjson_get_sint(yyjson_arr_get(spawn, i)));
            out.has_spawn = true;
        }
        out.imported = yyjson_get_bool(yyjson_obj_get(root, "imported"));
        // The time: absent in a world saved before there was any, which then begins at 0. A
        // clock is taken whole or left as new (ClockState.CODEC: a rate is positive).
        yyjson_val* time = yyjson_obj_get(root, "time");
        out.time.game_time = std::max<i64>(0, yyjson_get_sint(yyjson_obj_get(time, "game")));
        yyjson_val* clocks = yyjson_obj_get(time, "clocks");
        for (u32 i = 0; i < kClocks; ++i) {
            yyjson_val* c = yyjson_obj_getn(clocks, clock_name(i).data(), clock_name(i).size());
            if (!yyjson_is_obj(c)) continue;
            Clock read;
            read.total_ticks = yyjson_get_sint(yyjson_obj_get(c, "total_ticks"));
            read.partial_tick = static_cast<float>(yyjson_get_num(yyjson_obj_get(c, "partial_tick")));
            yyjson_val* rate = yyjson_obj_get(c, "rate");
            read.rate = rate ? static_cast<float>(yyjson_get_num(rate)) : 1.0f;
            read.paused = yyjson_get_bool(yyjson_obj_get(c, "paused"));
            if (read.total_ticks >= 0 && read.partial_tick >= 0.0f && read.partial_tick < 1.0f && read.rate > 0.0f &&
                std::isfinite(read.rate))
                out.time.clocks[i] = read;
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

bool write_meta(const std::string& path, const WorldInfo& info, std::string& err) {
    // Written beside it and moved into place: a world.json is whole or not there.
    const std::string tmp = path + ".tmp", json = world_json(info, info.time);
    FILE* f = std::fopen(tmp.c_str(), "wb");
    bool ok = f && std::fwrite(json.data(), 1, json.size(), f) == json.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) err = "cannot write " + path;
    return ok;
}

}  // namespace

std::string world_json(const WorldInfo& info, const WorldTime& now) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "format", kFormat);
    yyjson_mut_obj_add_int(doc, root, "data_version", proto::gen::kWorldVersion);
    yyjson_mut_obj_add_strcpy(doc, root, "version", std::string(proto::gen::kVersionName).c_str());
    yyjson_mut_obj_add_sint(doc, root, "seed", info.seed);
    yyjson_mut_val* gen = yyjson_mut_obj_add_obj(doc, root, "generator");
    yyjson_mut_obj_add_str(doc, gen, "type", info.flat ? "flat" : info.noise ? "noise" : "void");
    yyjson_mut_obj_add_strcpy(doc, gen, "biome", std::string(world::biome_name(info.biome)).c_str());
    yyjson_mut_obj_add_int(doc, gen, "sea_level", info.sea_level);
    if (info.flat) {
        yyjson_mut_val* layers = yyjson_mut_obj_add_arr(doc, gen, "layers");
        for (const world::FlatGenerator::Layer& l : info.layers) {
            yyjson_mut_val* layer = yyjson_mut_arr_add_obj(doc, layers);
            yyjson_mut_obj_add_strcpy(doc, layer, "block", std::string(world::block_name(world::block_of(l.state))).c_str());
            yyjson_mut_obj_add_int(doc, layer, "height", l.height);
        }
    }
    if (info.has_spawn) {
        yyjson_mut_val* spawn = yyjson_mut_obj_add_arr(doc, root, "spawn");
        for (const i32 v : info.spawn) yyjson_mut_arr_add_int(doc, spawn, v);
    }
    yyjson_mut_obj_add_bool(doc, root, "imported", info.imported);
    yyjson_mut_val* time = yyjson_mut_obj_add_obj(doc, root, "time");
    yyjson_mut_obj_add_sint(doc, time, "game", now.game_time);
    yyjson_mut_val* clocks = yyjson_mut_obj_add_obj(doc, time, "clocks");
    for (u32 i = 0; i < kClocks; ++i) {
        const Clock& c = now.clocks[i];
        yyjson_mut_val* clock = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_sint(doc, clock, "total_ticks", c.total_ticks);
        yyjson_mut_obj_add_real(doc, clock, "partial_tick", static_cast<double>(c.partial_tick));
        yyjson_mut_obj_add_real(doc, clock, "rate", static_cast<double>(c.rate));
        yyjson_mut_obj_add_bool(doc, clock, "paused", c.paused);
        yyjson_mut_obj_add(clocks, yyjson_mut_strncpy(doc, clock_name(i).data(), clock_name(i).size()), clock);
    }
    char* text = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string out = text ? text : "";
    std::free(text);
    yyjson_mut_doc_free(doc);
    return out;
}

WorldInfo::~WorldInfo() {
    if (lock_fd >= 0) ::close(lock_fd);
}

bool open_world(const Settings& s, base::Workers& workers, WorldInfo& out, std::string& err) {
    out.biome = static_cast<u16>(proto::reg::worldgen_biome::kPlains);
    if (s.level_name.empty()) {
        if (!worldgen::parse_seed(s.level_seed, out.seed)) out.seed = random_seed();
        if (!new_generator(s, out, err)) return false;
        LOG_INFO("level-name is empty: the world is not kept");
        return true;
    }
    out.dir = s.level_name;
    out.chunk_dir = out.dir + "/chunks/minecraft/overworld";
    std::error_code ec;
    std::filesystem::create_directories(out.dir, ec);
    if (ec) {
        err = "cannot make " + out.dir + ": " + ec.message();
        return false;
    }
    // DirectoryLock: vanilla's file and vanilla's lock (FileChannel.tryLock is fcntl's), so that
    // a world is run by one server, whichever kind.
    const std::string lock = out.dir + "/session.lock";
    out.lock_fd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (out.lock_fd < 0 || ::fcntl(out.lock_fd, F_SETLK, &fl) != 0) {
        err = "the world in " + out.dir + " is in use by another server (" + lock + " is locked)";
        return false;
    }

    const std::string meta = out.dir + "/world.json";
    if (std::filesystem::exists(meta, ec)) {
        bool seeded = false;
        if (!read_meta(meta, out, seeded, err)) return false;
        LOG_INFO("world %s: %s generator%s", out.dir.c_str(),
                 out.flat    ? "flat"
                 : out.noise ? "noise"
                             : "void",
                 out.imported ? ", imported from vanilla" : "");
        if (!seeded) out.seed = random_seed();  // a world from before worlds had seeds: it has this one from now on
        // (A world of terrain from before such worlds had a spawn gets one when the game begins: Game::find_spawn.)
        if (seeded) return true;
        return write_meta(meta, out, err);
    }
    // A world that is made here has the seed the settings name; one that vanilla made has its own.
    if (!worldgen::parse_seed(s.level_seed, out.seed)) out.seed = random_seed();
    if (std::filesystem::exists(out.dir + "/level.dat", ec)) {
        if (!import_world(out.dir, workers, out, err)) return false;
    } else {
        if (!new_generator(s, out, err)) return false;
        LOG_INFO("world %s: a new %s world", out.dir.c_str(), out.flat ? "flat" : "noise");
    }
    return write_meta(meta, out, err);
}

}  // namespace server
