// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/import.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <thread>

#include "base/clock.hpp"
#include "base/log.hpp"
#include "server/persist.hpp"
#include "world/block.hpp"
#include "world/chunk_io.hpp"
#include "world/region.hpp"

#include PROTO_GEN_DIMENSION_TYPES_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

namespace {

namespace be_type = proto::gen::block_entity_type;
namespace block_kind = proto::gen::block_kind;

constexpr u32 kAnvilSector = 4096;
constexpr u8 kGzip = 1, kZlib = 2, kStored = 3, kLz4 = 4, kExternal = 0x80;

bool read_file(const std::string& path, std::vector<u8>& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0;
    if (ok) {
        out.resize(static_cast<usize>(st.st_size));
        usize at = 0;
        while (at < out.size()) {
            const ssize_t n = ::read(fd, out.data() + at, out.size() - at);
            if (n <= 0) {
                ok = false;
                break;
            }
            at += static_cast<usize>(n);
        }
    }
    ::close(fd);
    return ok;
}

std::string_view text(nbt::Tag t) { return t.type == nbt::kString ? t.str() : std::string_view{}; }
// CompoundTag.getIntOr(name, 0): a number as an int, 0 for anything else - or for nothing.
i32 num(nbt::Tag t) { return t.numeric() ? t.as_int() : 0; }

// The tag in network form: its type, then the bytes nbt::Doc::write gives it.
void append_tag(nbt::Doc& doc, nbt::Tag t, base::ByteBuf& out) {
    const u8 type = t.type;
    out.append(&type, 1);
    doc.write(t, out);
}

// ---------------------------------------------------------------------------- a region file

// The bytes of chunk `index` of an Anvil region, inflated. 1: there they are; 0: no such chunk;
// -1: the file is not what it says (RegionFile.getChunkDataInputStream).
int anvil_chunk(const std::vector<u8>& file, const std::string& mca, i32 cx, i32 cz, u32 index, std::vector<u8>& out,
                std::string& err) {
    if (file.size() < 2 * kAnvilSector) return 0;
    const u32 location = load_be32(file.data() + 4 * index);
    if (location == 0) return 0;
    const u64 at = static_cast<u64>(location >> 8) * kAnvilSector;
    if (at + 5 > file.size()) {
        err = "its header points past the end of the file";
        return -1;
    }
    const u32 length = load_be32(file.data() + at);
    const u8 version = file[at + 4];
    if (length == 0) return 0;  // "allocated, but stream is missing"
    std::span<const u8> stored{file.data() + at + 5, length - 1};
    std::vector<u8> external;
    if (version & kExternal) {
        // A chunk too large for 255 sectors: c.<x>.<z>.mcc beside the region file.
        const std::string path =
            std::filesystem::path(mca).parent_path().string() + "/c." + std::to_string(cx) + "." + std::to_string(cz) + ".mcc";
        if (!read_file(path, external)) {
            err = path + " is missing";
            return -1;
        }
        stored = external;
    } else if (at + 4 + length > file.size()) {
        err = "it ends past the end of the file";
        return -1;
    }
    switch (version & ~kExternal) {
        case kGzip:
            if (world::inflate_gzip(stored, out)) return 1;
            break;
        case kZlib:
            if (world::inflate_zlib(stored, out)) return 1;
            break;
        case kStored: out.assign(stored.begin(), stored.end()); return 1;
        case kLz4:
            err =
                "it is compressed with lz4, which this server does not read: set region-file-compression=deflate "
                "and let vanilla save the world again (its forceUpgrade rewrites every chunk)";
            return -1;
        default: break;
    }
    if (err.empty()) err = "it does not decompress";
    return -1;
}

// ---------------------------------------------------------------------------- a chunk

// BlockState.CODEC: a block's name for its default state, or {id, properties}. A property that is
// missing, or names no value, has the default state's (StateDefinition.appendPropertyCodec).
bool state_of(nbt::Tag t, world::BlockState& out, std::string& err) {
    nbt::Tag props{};
    std::string_view name = text(t);
    if (t.type == nbt::kCompound) {
        name = text(t.get("id"));
        props = t.get("properties");
    }
    const u32 block = world::find_block(name);
    if (block == world::kNoId) {
        err = "a block that does not exist: '" + std::string(name) + "'";
        return false;
    }
    const world::BlockInfo& info = world::block_info(block);
    out = info.default_state;
    if (props.type != nbt::kCompound) return true;
    const u32 offset = static_cast<u32>(info.default_state - info.first_state);
    u32 state = info.first_state;
    for (u32 i = 0; i < world::block_property_count(block); ++i) {
        const world::BlockProperty p = world::block_property(block, i);
        u32 index = offset / p.stride % static_cast<u32>(p.values.size());
        const std::string_view value = text(props.get(p.name));
        for (u32 k = 0; k < p.values.size(); ++k)
            if (p.values[k] == value) index = k;
        state += index * p.stride;
    }
    out = static_cast<world::BlockState>(state);
    return true;
}

// One PalettedContainer as SimpleBitStorage packs it: `bits` wide entries, 64 / bits to a long.
// Calls put(i, palette index) for each of `count` entries; false for an index past the palette.
template <class Put>
bool unpack_longs(nbt::Tag data, u32 bits, u32 count, u32 palette, Put&& put) {
    if (data.type != nbt::kLongArray || data.size() != world::words_for(count, bits)) return false;
    const u8* longs = data.payload() + 4;
    const u32 per = 64 / bits;
    const u64 mask = (1ull << bits) - 1;
    for (u32 i = 0, w = 0; i < count; ++w) {
        const u64 v = load_be64(longs + 8ull * w);
        for (u32 k = 0; k < per && i < count; ++k, ++i) {
            const u32 index = static_cast<u32>((v >> (k * bits)) & mask);
            if (index >= palette) return false;
            put(i, index);
        }
    }
    return true;
}

// block_states: a palette of states and, for more than one, indices of max(4, ceillog2(n)) bits
// (Strategy.createForBlockStates).
bool read_blocks(nbt::Tag container, world::Section& section, world::SectionAllocator& alloc, std::string& err) {
    const nbt::Tag palette = container.get("palette");
    if (palette.type != nbt::kList || palette.size() == 0) {
        err = "block states without a palette";
        return false;
    }
    std::vector<world::BlockState> states(palette.size());
    u32 n = 0;
    for (const nbt::Tag entry : palette.elements())
        if (!state_of(entry, states[n++], err)) return false;
    if (n == 1) {
        section.fill(alloc, states[0]);
        return true;
    }
    world::BlockState all[world::Section::kVolume];
    const u32 bits = std::max<u32>(4, static_cast<u32>(std::bit_width(n - 1)));
    if (!unpack_longs(container.get("data"), bits, world::Section::kVolume, n,
                      [&](u32 i, u32 index) { all[i] = states[index]; })) {
        err = "block states whose data does not fit their palette";
        return false;
    }
    section.assign(alloc, all);
    return true;
}

// biomes: the same over 64 cells, ceillog2(n) bits (Strategy.createForBiomes).
bool read_biomes(nbt::Tag container, world::Chunk& c, u32 si, world::ChunkArena& arena, std::string& err) {
    const nbt::Tag palette = container.get("palette");
    if (palette.type != nbt::kList || palette.size() == 0 || palette.size() > 64) {
        err = "biomes without a palette, or with more of it than cells";
        return false;
    }
    u16 ids[64];
    u32 n = 0;
    for (const nbt::Tag entry : palette.elements()) {
        const u32 id = world::find_biome(text(entry));
        if (id == world::kNoId) {
            err = "a biome that does not exist: '" + std::string(text(entry)) + "'";
            return false;
        }
        ids[n++] = static_cast<u16>(id);
    }
    c.biome[si] = ids[0];
    if (n == 1) return true;
    if (!c.biomes)
        c.biomes = new (arena.alloc(sizeof(world::BiomeSection) * world::kMaxSections)) world::BiomeSection[world::kMaxSections];
    world::BiomeSection& b = c.biomes[si];
    if (!unpack_longs(container.get("data"), static_cast<u32>(std::bit_width(n - 1)), 64, n,
                      [&](u32 i, u32 index) { b.index[i] = static_cast<u8>(index); })) {
        err = "biomes whose data does not fit their palette";
        return false;
    }
    b.count = static_cast<u8>(n);
    std::copy_n(ids, n, b.palette);
    return true;
}

// SkyLight / BlockLight: 2048 bytes, a DataLayer's own layout.
bool read_light(nbt::Tag bytes, world::LightLayer& layer, world::ChunkArena& arena, std::string& err) {
    if (!bytes) return true;
    if (bytes.type != nbt::kByteArray || bytes.size() != world::LightLayer::kBytes) {
        err = "a light layer that is not 2048 bytes";
        return false;
    }
    u8* a = static_cast<u8*>(arena.alloc(world::LightLayer::kBytes));
    std::memcpy(a, bytes.payload() + 4, world::LightLayer::kBytes);
    layer.v = reinterpret_cast<uintptr_t>(a);
    return true;
}

// ---------------------------------------------------------------------------- block entities

struct PlainStack {
    u32 item = 0, count = 0;
};

bool is_number(nbt::Tag t) { return t.numeric(); }
bool empty_compound(nbt::Tag t) { return t.type == nbt::kCompound && t.size() == 0; }

// "Items" as ContainerHelper.loadAllItems reads it, if every stack is one this server holds as
// vanilla does: an item by its name, a count, no components.
bool plain_items(nbt::Tag items, PlainStack* stacks, u32 slots) {
    if (!items) return true;
    if (items.type != nbt::kList) return false;
    for (const nbt::Tag stack : items.elements()) {
        if (stack.type != nbt::kCompound) return false;
        i64 slot = -1, count = 1;
        u32 item = world::kNoId;
        for (const nbt::Tag e : stack.entries()) {
            const std::string_view key = e.name();
            if (key == "Slot" && is_number(e)) {
                slot = e.as_long();
            } else if (key == "id" && e.type == nbt::kString) {
                item = world::find_item(e.str());
            } else if (key == "count" && is_number(e)) {
                count = e.as_long();
            } else if (!(key == "components" && empty_compound(e))) {
                return false;
            }
        }
        if (slot < 0 || slot >= slots || item == world::kNoId || item == 0 || count < 1 || count > 99) return false;
        if (stacks[slot].count) return false;  // a slot named twice: the first would be lost
        stacks[slot] = {item, static_cast<u32>(count)};
    }
    return true;
}

struct Converted {
    base::ByteBuf records;
    u32 count = 0;
};

// One entry of block_entities. Where the block has no block entity, or the tag names no type,
// vanilla drops it when it loads the chunk (LevelChunk.setBlockEntity, BlockEntity.loadStatic):
// so does this.
void convert_block_entity(nbt::Doc& doc, nbt::Tag tag, const world::Chunk& c, const world::Layout& l, Converted& out,
                          ImportStats& stats) {
    if (tag.type != nbt::kCompound) return;
    const u32 type = world::find_block_entity_type(text(tag.get("id")));
    // BlockEntity.getPosFromTag: one found in the wrong chunk is taken to be in this one.
    const u32 lx = static_cast<u32>(num(tag.get("x"))) & 15, lz = static_cast<u32>(num(tag.get("z"))) & 15;
    const i32 y = num(tag.get("y"));
    const u32 si = static_cast<u32>((y - l.min_y) >> 4);
    if (type == world::kNoId || si >= l.sections) return;
    const world::BlockState state = c.sections[si].get(lx, static_cast<u32>(y & 15), lz);
    if (!(world::flags_of(state) & world::state::kBlockEntity)) return;
    const u32 kind = world::block_info_of(state).kind;
    const u8 xz = static_cast<u8>(lx << 4 | lz);

    const bool chest = kind == block_kind::kChest && type == be_type::kChest;
    const bool furnace = kind == block_kind::kFurnace && type == be_type::kFurnace;
    PlainStack stacks[27];
    i32 timers[4] = {0, 0, 0, 0};  // lit remaining, lit total, cooking, cooking total: AbstractFurnaceBlockEntity.dataAccess
    nbt::Tag recipes{};
    bool plain = chest || furnace;
    if (plain) {
        // Every entry is one that is read here, or the block entity is kept as it is: a name, a
        // lock, a loot table, components change what vanilla does with it.
        for (const nbt::Tag e : tag.entries()) {
            const std::string_view key = e.name();
            if (key == "id" || key == "x" || key == "y" || key == "z") continue;
            if (key == "keepPacked" && is_number(e) && e.as_int() == 0) continue;
            if (key == "components" && empty_compound(e)) continue;
            if (key == "Items" && plain_items(e, stacks, chest ? 27 : 3)) continue;
            if (furnace && is_number(e)) {
                if (key == "lit_time_remaining") {
                    timers[0] = e.as_int();
                    continue;
                }
                if (key == "lit_total_time") {
                    timers[1] = e.as_int();
                    continue;
                }
                if (key == "cooking_time_spent") {
                    timers[2] = e.as_int();
                    continue;
                }
                if (key == "cooking_total_time") {
                    timers[3] = e.as_int();
                    continue;
                }
                if (key == "speed_multiplier" && e.as_double() == 1.0) continue;
            }
            // What a furnace has cooked - the experience it owes - changes nothing it does here: kept beside it.
            if (furnace && key == "RecipesUsed" && e.type == nbt::kCompound) {
                recipes = e;
                continue;
            }
            plain = false;
            break;
        }
    }
    ++out.count;
    if (!plain) {
        record::put_block_entity(out.records, xz, y, type, record::kKept);
        base::ByteBuf kept;
        append_tag(doc, tag, kept);
        record::put_counted(out.records, {kept.data, kept.len});
        ++stats.kept_block_entities;
        return;
    }
    ++stats.block_entities;
    record::put_block_entity(out.records, xz, y, type, chest ? record::kChest : record::kFurnace);
    for (u32 i = 0; i < (chest ? 27u : 3u); ++i) record::put_plain_stack(out.records, stacks[i].item, stacks[i].count);
    if (chest) {
        record::put_counted(out.records, {});
        return;
    }
    u8* p = out.records.reserve(16);
    for (const i32 t : timers) p = proto::put_be32(p, static_cast<u32>(t));
    out.records.commit(p);
    base::ByteBuf kept;
    if (recipes && recipes.size()) {
        base::ByteBuf payload;
        doc.write(recipes, payload);
        nbt::Writer w(kept);
        w.root(nbt::kCompound);
        w.put_raw("RecipesUsed", nbt::kCompound, {payload.data, payload.len});
        w.end_compound();
    }
    record::put_counted(out.records, {kept.data, kept.len});
}

void put_u32(base::ByteBuf& out, u32 v) {
    u8* p = out.reserve(4);
    store_le<u32>(p, v);
    out.commit(p + 4);
}

}  // namespace

bool import_chunk(nbt::Doc& terrain_doc, nbt::Doc& entities_doc, i32 cx, i32 cz, const world::Layout& l, base::ByteBuf& out,
                  ImportStats& stats, std::string& err) {
    const nbt::Tag terrain = terrain_doc.root(), entity_root = entities_doc.root();
    const auto version_ok = [&](nbt::Tag root) {
        const i32 version = num(root.get("DataVersion"));
        if (version == proto::gen::kWorldVersion) return true;
        err = "it was saved by data version " + std::to_string(version) + ", this server reads " +
              std::to_string(proto::gen::kWorldVersion) + " (" + std::string(proto::gen::kVersionName) +
              "): open the world in that version of vanilla first";
        return false;
    };
    if (terrain.type == nbt::kCompound && !version_ok(terrain)) return false;
    if (entity_root.type == nbt::kCompound && !version_ok(entity_root)) return false;

    // What is kept unread: the chunk's other entries, and its entities.
    static constexpr std::string_view kRead[] = {"DataVersion", "Status",         "xPos",       "yPos",     "zPos",
                                                 "sections",    "block_entities", "Heightmaps", "isLightOn"};
    const bool full = terrain.type == nbt::kCompound && text(terrain.get("Status")) == "minecraft:full";
    base::ByteBuf kept, other, tags;
    u32 entities = 0;
    if (full) {
        nbt::Writer w(other);
        w.root(nbt::kCompound);
        base::ByteBuf payload;
        u32 n = 0;
        for (const nbt::Tag e : terrain.entries()) {
            if (std::find(std::begin(kRead), std::end(kRead), e.name()) != std::end(kRead)) continue;
            payload.clear();
            terrain_doc.write(e, payload);
            w.put_raw(e.name(), e.type, {payload.data, payload.len});
            ++n;
        }
        w.end_compound();
        if (n == 0) other.clear();
    }
    if (const nbt::Tag list = entity_root.type == nbt::kCompound ? entity_root.get("Entities") : nbt::Tag{};
        list.type == nbt::kList) {
        base::ByteBuf one;
        for (const nbt::Tag e : list.elements()) {
            if (e.type != nbt::kCompound) continue;
            one.clear();
            append_tag(entities_doc, e, one);
            put_u32(tags, one.len);
            tags.append(one.data, one.len);
            ++entities;
        }
    }
    if (other.len || entities) {
        const u8 version = 1;
        kept.append(&version, 1);
        put_u32(kept, other.len);
        if (other.len) kept.append(other.data, other.len);
        put_u32(kept, entities);
        if (tags.len) kept.append(tags.data, tags.len);
    }
    stats.entities += entities;

    world::LoadedChunk lc;
    world::Chunk& c = lc.chunk;
    c.x = cx, c.z = cz;
    c.sections_count = static_cast<u8>(l.sections);
    c.light_count = static_cast<u8>(l.sections + 2);
    c.keep = {kept.len, kept.data};
    if (!full) {
        // Nothing of its blocks: only what is kept, if anything is.
        if (terrain.type == nbt::kCompound) ++stats.unfinished;
        if (kept.len) world::encode_kept(c, out);
        return true;
    }
    for (u32 si = 0; si < l.sections; ++si)
        c.biome[si] = static_cast<u16>(proto::reg::worldgen_biome::kPlains);  // createForBiomes' default
    const nbt::Tag sections = terrain.get("sections");
    if (sections.type == nbt::kList) {
        for (const nbt::Tag s : sections.elements()) {
            if (s.type != nbt::kCompound) continue;
            const i32 y = static_cast<i8>(num(s.get("Y")));
            const u32 si = static_cast<u32>(y - l.min_section), li = si + 1;
            if (si < l.sections) {
                if (const nbt::Tag blocks = s.get("block_states"); blocks.type == nbt::kCompound) {
                    if (!read_blocks(blocks, c.sections[si], lc.alloc, err)) return false;
                }
                if (const nbt::Tag biomes = s.get("biomes"); biomes.type == nbt::kCompound) {
                    if (!read_biomes(biomes, c, si, lc.arena, err)) return false;
                }
            }
            if (li < l.sections + 2) {
                if (!read_light(s.get("BlockLight"), c.block[li], lc.arena, err)) return false;
                if (l.has_skylight && !read_light(s.get("SkyLight"), c.sky[li], lc.arena, err)) return false;
            }
        }
    }
    // ChunkStatusTasks.isLighted: a full chunk whose light is on is not lit again.
    c.status = num(terrain.get("isLightOn")) != 0 ? world::ChunkStatus::Lit : world::ChunkStatus::LightReady;
    c.modified = true;  // not what any generator here would make

    Converted converted;
    if (const nbt::Tag list = terrain.get("block_entities"); list.type == nbt::kList) {
        for (const nbt::Tag be : list.elements()) convert_block_entity(terrain_doc, be, c, l, converted, stats);
    }
    base::ByteBuf block_entities;
    if (converted.count) {
        record::put_block_entities_head(block_entities, converted.count);
        block_entities.append(converted.records.data, converted.records.len);
    }
    world::encode_chunk(c, {{block_entities.data, block_entities.len}, {}}, out);
    ++stats.chunks;
    return true;
}

bool import_region(const std::string& mca, const std::string& entities_mca, i32 rx, i32 rz, const std::string& out_path,
                   const world::Layout& layout, ImportStats& stats, std::string& err) {
    std::vector<u8> terrain_file, entity_file;
    const bool has_terrain = read_file(mca, terrain_file), has_entities = read_file(entities_mca, entity_file);
    if (!has_terrain && !has_entities) return true;

    world::RegionFile region;
    std::vector<std::vector<u8>> blobs;
    std::vector<world::RegionFile::Put> batch;
    usize batched = 0;
    const auto flush = [&]() {
        if (batch.empty()) return true;
        if (region.fd < 0 && !region.open(out_path, true, err)) return false;
        if (!region.write(batch, false, err)) return false;
        batch.clear();
        blobs.clear();
        batched = 0;
        return true;
    };
    std::vector<u8> terrain_bytes, entity_bytes;
    base::ByteBuf chunk;
    for (u32 index = 0; index < world::kRegionChunks; ++index) {
        const i32 cx = rx * 32 + static_cast<i32>(index & 31), cz = rz * 32 + static_cast<i32>(index >> 5);
        const auto fail = [&](const std::string& file) {
            err = file + ", chunk " + std::to_string(cx) + ", " + std::to_string(cz) + ": " + err;
            return false;
        };
        nbt::Doc terrain, entities;
        if (has_terrain) {
            const int got = anvil_chunk(terrain_file, mca, cx, cz, index, terrain_bytes, err);
            if (got < 0) return fail(mca);
            if (got > 0 && terrain.parse(terrain_bytes, nbt::Form::kNamed, nbt::kTrusted) != nbt::Error::kNone) {
                err = "it is not NBT";
                return fail(mca);
            }
        }
        if (has_entities) {
            const int got = anvil_chunk(entity_file, entities_mca, cx, cz, index, entity_bytes, err);
            if (got < 0) return fail(entities_mca);
            if (got > 0 && entities.parse(entity_bytes, nbt::Form::kNamed, nbt::kTrusted) != nbt::Error::kNone) {
                err = "it is not NBT";
                return fail(entities_mca);
            }
        }
        if (!terrain.root() && !entities.root()) continue;
        chunk.clear();
        if (!import_chunk(terrain, entities, cx, cz, layout, chunk, stats, err)) return fail(terrain.root() ? mca : entities_mca);
        if (chunk.len == 0) continue;
        blobs.emplace_back(chunk.data, chunk.data + chunk.len);
        batch.push_back({index, {}});
        batched += chunk.len;
        // (The spans are set when the batch goes out: `blobs` may still move its vectors.)
        if (batched >= (32u << 20)) {
            for (usize i = 0; i < batch.size(); ++i) batch[i].raw = blobs[i];
            if (!flush()) return false;
        }
    }
    for (usize i = 0; i < batch.size(); ++i) batch[i].raw = blobs[i];
    if (!flush()) return false;
    if (region.fd >= 0 && ::fdatasync(region.fd) != 0) {
        err = out_path + ": cannot be synced";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------- a world

namespace {

struct RegionJob {
    std::string mca, entities_mca, out;
    i32 rx, rz;
    world::Layout layout;
    ImportStats stats;
    std::string err;
    bool ok = false;
    // the owner's
    struct Shared* shared;
};
struct Shared {
    ImportStats stats;
    std::string err;
    u32 done = 0;
};

void region_work(base::Task& t) {
    auto* job = static_cast<RegionJob*>(t.data);
    job->ok = import_region(job->mca, job->entities_mca, job->rx, job->rz, job->out, job->layout, job->stats, job->err);
}
void region_done(base::Task& t) {
    auto* job = static_cast<RegionJob*>(t.data);
    job->shared->stats.add(job->stats);
    ++job->shared->done;
    if (!job->ok && job->shared->err.empty()) job->shared->err = job->err;
    delete job;
}

bool read_nbt_gz(const std::string& path, std::vector<u8>& bytes, nbt::Doc& doc) {
    std::vector<u8> file;
    return read_file(path, file) && world::inflate_gzip(file, bytes) &&
           doc.parse(bytes, nbt::Form::kNamed, nbt::kTrusted) == nbt::Error::kNone;
}

}  // namespace

bool import_world(const std::string& dir, base::Workers& workers, WorldInfo& info, std::string& err) {
    const u64 t0 = base::now_ns();
    std::vector<u8> bytes;
    nbt::Doc level;
    if (!read_nbt_gz(dir + "/level.dat", bytes, level)) {
        err = dir + "/level.dat cannot be read";
        return false;
    }
    const nbt::Tag data = level.root().get("Data");
    if (const i32 version = num(data.get("DataVersion")); version != proto::gen::kWorldVersion) {
        err = dir + " was saved by data version " + std::to_string(version) + ", this server reads " +
              std::to_string(proto::gen::kWorldVersion) + " (" + std::string(proto::gen::kVersionName) +
              "): open the world in that version of vanilla first";
        return false;
    }
    // LevelData.RespawnData: where the world's spawn is.
    if (const nbt::Tag spawn = data.get("spawn"); spawn.type == nbt::kCompound) {
        const nbt::Tag pos = spawn.get("pos");
        const std::string_view dimension = text(spawn.get("dimension"));
        if (pos.type == nbt::kIntArray && pos.size() == 3 && (dimension.empty() || dimension == "minecraft:overworld")) {
            u32 i = 0;
            for (const nbt::Tag v : pos.elements()) info.spawn[i++] = v.as_int();
            info.has_spawn = true;
        }
    }
    // The overworld's generator (data/minecraft/world_gen_settings.dat). A flat world is made as
    // it was made there; for vanilla's terrain there is no generator here.
    info.flat = false;
    info.sea_level = 63;  // data/minecraft/worldgen/noise_settings/overworld.json
    info.biome = static_cast<u16>(proto::reg::worldgen_biome::kTheVoid);
    std::vector<u8> gen_bytes;
    nbt::Doc gen_doc;
    if (read_nbt_gz(dir + "/data/minecraft/world_gen_settings.dat", gen_bytes, gen_doc)) {
        const nbt::Tag generator = gen_doc.root().get("data").get("dimensions").get("minecraft:overworld").get("generator");
        if (generator.type == nbt::kCompound && text(generator.get("type")) == "minecraft:flat") {
            const nbt::Tag settings = generator.get("settings"), layers = settings.get("layers");
            std::vector<world::FlatGenerator::Layer> made;
            bool ok = layers.type == nbt::kList;
            if (ok) {
                for (const nbt::Tag layer : layers.elements()) {
                    const u32 block = world::find_block(text(layer.get("block")));
                    const i32 height = num(layer.get("height"));
                    if (block == world::kNoId || height < 0 || height > 4064) {
                        ok = false;
                        break;
                    }
                    made.push_back({world::block_info(block).default_state, static_cast<u32>(height)});
                }
            }
            const u32 biome = world::find_biome(text(settings.get("biome")));
            if (ok) {
                info.flat = true;
                info.layers = std::move(made);
                info.sea_level = -63;  // FlatLevelSource.getSeaLevel()
                info.biome = static_cast<u16>(biome == world::kNoId ? proto::reg::worldgen_biome::kPlains : biome);
            }
        }
    }

    // The regions: every r.X.Z.mca of the overworld's blocks or of its entities.
    const std::string overworld = dir + "/dimensions/minecraft/overworld";
    std::set<std::pair<i32, i32>> regions;
    std::error_code ec;
    for (const char* sub : {"/region", "/entities"}) {
        for (std::filesystem::directory_iterator it(overworld + sub, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            int rx = 0, rz = 0, used = 0;
            if (std::sscanf(name.c_str(), "r.%d.%d.mca%n", &rx, &rz, &used) == 2 && static_cast<usize>(used) == name.size())
                regions.insert({rx, rz});
        }
    }
    if (regions.empty() && std::filesystem::exists(dir + "/region", ec)) {
        err = dir + " has the layout of a world from before 26.1 (region/ at the top): open it in vanilla " +
              std::string(proto::gen::kVersionName) + " first";
        return false;
    }
    std::filesystem::create_directories(info.chunk_dir, ec);
    const proto::gen::dimension::Type& type = proto::gen::dimension::kTypes[proto::reg::dimension_type::kOverworld];
    const world::Layout layout{type.min_y, type.min_y >> 4, static_cast<u32>(type.height) / 16, type.has_skylight};
    LOG_INFO("importing the vanilla world in %s: %zu region(s)", dir.c_str(), regions.size());
    Shared shared;
    for (const auto& [rx, rz] : regions) {
        const std::string name = "/r." + std::to_string(rx) + "." + std::to_string(rz);
        auto* job = new RegionJob{overworld + "/region" + name + ".mca",
                                  overworld + "/entities" + name + ".mca",
                                  info.chunk_dir + name + ".rgn",
                                  rx,
                                  rz,
                                  layout,
                                  {},
                                  {},
                                  false,
                                  &shared};
        workers.submit({&region_work, &region_done, nullptr, job, 0, 0, 0, 0}, 1);
    }
    workers.kick();
    u64 said = base::now_ns();
    while (workers.in_flight()) {
        if (workers.poll() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (base::now_ns() - said > 5 * base::kNsPerSec) {
            said = base::now_ns();
            LOG_INFO("  ...%u of %zu regions, %llu chunks", shared.done, regions.size(),
                     static_cast<unsigned long long>(shared.stats.chunks));
        }
    }
    if (!shared.err.empty()) {
        err = "import failed: " + shared.err;
        return false;
    }
    const ImportStats& st = shared.stats;
    LOG_INFO(
        "imported %llu chunks in %.1f s (%llu more were not finished by vanilla and are left to the generator); "
        "%llu chests and furnaces, %llu other block entities and %llu entities kept as they are",
        static_cast<unsigned long long>(st.chunks), static_cast<double>(base::now_ns() - t0) / 1e9,
        static_cast<unsigned long long>(st.unfinished), static_cast<unsigned long long>(st.block_entities),
        static_cast<unsigned long long>(st.kept_block_entities), static_cast<unsigned long long>(st.entities));
    for (const char* other : {"/dimensions/minecraft/the_nether/region", "/dimensions/minecraft/the_end/region"}) {
        if (std::filesystem::exists(dir + other, ec) && !std::filesystem::is_empty(dir + other, ec))
            LOG_WARN("%s%s is not imported: this server has one dimension, the overworld", dir.c_str(), other);
    }
    info.imported = true;
    return true;
}

}  // namespace server
