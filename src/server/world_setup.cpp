// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <unistd.h>
#include <yyjson.h>

#include <cstdio>
#include <filesystem>

#include "base/log.hpp"
#include "server/import.hpp"
#include "server/world_info.hpp"
#include "world/block.hpp"

namespace server {

namespace {

constexpr i64 kFormat = 1;  // of world.json and of the chunk bytes (world/chunk_io.hpp)

bool read_meta(const std::string& path, WorldInfo& out, std::string& err) {
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
        yyjson_val* gen = yyjson_obj_get(root, "generator");
        const char* type = yyjson_get_str(yyjson_obj_get(gen, "type"));
        out.flat = !type || std::string_view(type) == "flat";
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
    }
    yyjson_doc_free(doc);
    return ok;
}

bool write_meta(const std::string& path, const WorldInfo& info, std::string& err) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "format", kFormat);
    yyjson_mut_obj_add_int(doc, root, "data_version", proto::gen::kWorldVersion);
    yyjson_mut_obj_add_strcpy(doc, root, "version", std::string(proto::gen::kVersionName).c_str());
    yyjson_mut_val* gen = yyjson_mut_obj_add_obj(doc, root, "generator");
    yyjson_mut_obj_add_str(doc, gen, "type", info.flat ? "flat" : "void");
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
    // Written beside it and moved into place: a world.json is whole or not there.
    const std::string tmp = path + ".tmp";
    yyjson_write_err werr;
    bool ok = yyjson_mut_write_file(tmp.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &werr);
    if (!ok) err = "cannot write " + tmp + ": " + werr.msg;
    yyjson_mut_doc_free(doc);
    if (ok && std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "cannot write " + path;
        ok = false;
    }
    return ok;
}

}  // namespace

WorldInfo::~WorldInfo() {
    if (lock_fd >= 0) ::close(lock_fd);
}

bool open_world(const Settings& s, base::Workers& workers, WorldInfo& out, std::string& err) {
    out.biome = static_cast<u16>(proto::reg::worldgen_biome::kPlains);
    if (s.level_name.empty()) {
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
        if (!read_meta(meta, out, err)) return false;
        LOG_INFO("world %s: %s generator%s", out.dir.c_str(), out.flat ? "flat" : "void",
                 out.imported ? ", imported from vanilla" : "");
        return true;
    }
    if (std::filesystem::exists(out.dir + "/level.dat", ec)) {
        if (!import_world(out.dir, workers, out, err)) return false;
    } else {
        LOG_INFO("world %s: a new flat world", out.dir.c_str());
    }
    return write_meta(meta, out, err);
}

}  // namespace server
