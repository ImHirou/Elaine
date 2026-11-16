#include "world/chunk_io.hpp"

namespace world {

namespace {

constexpr u8 kVersion = 1;
constexpr u8 kLit = 1, kModified = 2, kTerrain = 4;
enum : u8 { kAbsent = 0, kUniform = 1, kFolded = 2, kArray = 3 };

void put_layer(base::ByteBuf& out, const LightLayer& l) {
    u8* p = out.reserve(2 + LightLayer::kBytes);
    if (!l.present()) {
        *p++ = kAbsent;
    } else if (l.has_array()) {
        *p++ = kArray;
        std::memcpy(p, l.array(), LightLayer::kBytes);
        p += LightLayer::kBytes;
    } else {
        *p++ = l.v <= 16 ? kUniform : kFolded;
        *p++ = static_cast<u8>(l.uniform_value());
    }
    out.commit(p);
}

// A reader that never runs past its end: a short blob makes every later read fail.
struct In {
    const u8* p;
    const u8* end;
    bool ok = true;
    const u8* take(usize n) {
        if (UNLIKELY(static_cast<usize>(end - p) < n)) {
            ok = false;
            p = end;
            return nullptr;
        }
        const u8* at = p;
        p += n;
        return at;
    }
    template <class T>
    T num() {
        const u8* at = take(sizeof(T));
        return at ? load_le<T>(at) : T{};
    }
    std::span<const u8> bytes() {
        const u32 n = num<u32>();
        const u8* at = take(n);
        return at ? std::span<const u8>{at, n} : std::span<const u8>{};
    }
};

bool get_layer(In& in, ChunkArena& arena, LightLayer& l) {
    const u8 kind = in.num<u8>();
    if (kind == kAbsent) return in.ok;
    if (kind == kArray) {
        const u8* src = in.take(LightLayer::kBytes);
        if (!src) return false;
        u8* a = static_cast<u8*>(arena.alloc(LightLayer::kBytes));
        std::memcpy(a, src, LightLayer::kBytes);
        l.v = reinterpret_cast<uintptr_t>(a);
        return true;
    }
    const u8 value = in.num<u8>();
    if (!in.ok || value > 15 || (kind != kUniform && kind != kFolded)) return false;
    l.v = (kind == kUniform ? 1u : 17u) + value;
    return true;
}

std::span<const u8> copy_into(ChunkArena& arena, std::span<const u8> bytes) {
    if (bytes.empty()) return {};
    u8* to = static_cast<u8*>(arena.alloc(bytes.size()));
    std::memcpy(to, bytes.data(), bytes.size());
    return {to, bytes.size()};
}

}  // namespace

void encode_chunk(const Chunk& c, const ChunkParts& parts, base::ByteBuf& out) {
    u8* p = out.reserve(16);
    *p++ = kVersion;
    *p++ = static_cast<u8>((c.status == ChunkStatus::Lit ? kLit : 0) | (c.modified ? kModified : 0) | kTerrain);
    store_le<i32>(p, c.x);
    store_le<i32>(p + 4, c.z);
    p[8] = c.sections_count;
    p[9] = c.light_count;
    out.commit(p + 10);
    for (u32 si = 0; si < c.sections_count; ++si) {
        const Section& s = c.sections[si];
        const usize body = s.bits == 0                      ? 2
                           : s.bits == Section::kDirectBits ? Section::kVolume * 2
                                                            : 2 + 2u * s.palette_len + packed_bytes(s.bits);
        p = out.reserve(1 + body + 3 + 64 * 3);
        *p++ = s.bits;
        if (s.bits == 0) {
            store_le<u16>(p, s.single);
            p += 2;
        } else if (s.bits == Section::kDirectBits) {
            std::memcpy(p, s.data, Section::kVolume * 2);
            p += Section::kVolume * 2;
        } else {
            store_le<u16>(p, s.palette_len);
            std::memcpy(p + 2, s.palette, 2u * s.palette_len);
            p += 2 + 2u * s.palette_len;
            std::memcpy(p, s.data, packed_bytes(s.bits));
            p += packed_bytes(s.bits);
        }
        store_le<u16>(p, c.biome[si]);
        p += 2;
        const BiomeSection* b = c.biomes ? &c.biomes[si] : nullptr;
        *p++ = b ? b->count : 0;
        if (b && b->count) {
            std::memcpy(p, b->palette, 2u * b->count);
            p += 2u * b->count;
            std::memcpy(p, b->index, 64);
            p += 64;
        }
        out.commit(p);
    }
    for (u32 li = 0; li < c.light_count; ++li) {
        put_layer(out, c.sky_layer(li));
        put_layer(out, c.block_layer(li));
    }
    const std::span<const u8> tail[3] = {parts.block_entities, parts.entities, {c.keep.bytes, c.keep.len}};
    for (const std::span<const u8> part : tail) {
        p = out.reserve(4 + part.size());
        store_le<u32>(p, static_cast<u32>(part.size()));
        if (!part.empty()) std::memcpy(p + 4, part.data(), part.size());
        out.commit(p + 4 + part.size());
    }
}

void encode_kept(const Chunk& c, base::ByteBuf& out) {
    u8* p = out.reserve(10 + 12 + c.keep.len);
    *p++ = kVersion;
    *p++ = 0;
    store_le<i32>(p, c.x);
    store_le<i32>(p + 4, c.z);
    p[8] = c.sections_count;
    p[9] = c.light_count;
    store_le<u32>(p + 10, 0);  // block entities
    store_le<u32>(p + 14, 0);  // entities
    store_le<u32>(p + 18, c.keep.len);
    if (c.keep.len) std::memcpy(p + 22, c.keep.bytes, c.keep.len);
    out.commit(p + 22 + c.keep.len);
}

bool load_chunk(std::span<const u8> blob, i32 cx, i32 cz, const Layout& l, ChunkGenerator& gen, LoadedChunk& out,
                std::string& err) {
    Chunk& c = out.chunk;
    c.x = cx;
    c.z = cz;
    c.sections_count = static_cast<u8>(l.sections);
    c.light_count = static_cast<u8>(l.sections + 2);
    const auto fail = [&](const char* what) {
        err = what;
        return false;
    };
    bool terrain = false;
    In in{blob.data(), blob.data() + blob.size()};
    if (!blob.empty()) {
        const u8 version = in.num<u8>(), flags = in.num<u8>();
        const i32 x = in.num<i32>(), z = in.num<i32>();
        const u8 sections = in.num<u8>(), light = in.num<u8>();
        if (!in.ok || version != kVersion) return fail("not a chunk of a version this server reads");
        if (x != cx || z != cz) return fail("the chunk of another place");
        if (sections != l.sections || light != l.sections + 2) return fail("a chunk of another height");
        terrain = (flags & kTerrain) != 0;
        c.modified = (flags & kModified) != 0;
        c.loaded_lit = terrain && (flags & kLit) != 0;
    }
    if (!terrain) {
        gen.generate(c, out.alloc);
        out.fresh = blob.empty();
        c.dirty = true;  // nothing on disk holds these blocks
    } else {
        for (u32 si = 0; si < l.sections; ++si) {
            Section& s = c.sections[si];
            const u8 bits = in.num<u8>();
            if (bits == 0) {
                const u16 state = in.num<u16>();
                if (!in.ok || state >= state::kCount) return fail("a block state that does not exist");
                s.fill(out.alloc, state);
            } else if (bits == Section::kDirectBits) {
                const u8* data = in.take(Section::kVolume * 2);
                if (!data || !s.load(out.alloc, bits, nullptr, 0, data)) return fail("a section that is not one");
            } else {
                const u16 n = in.num<u16>();
                u16 pal[256];
                const u8* pal_bytes = bits <= Section::kMaxPaletteBits && n <= 256 ? in.take(2u * n) : nullptr;
                const u8* data = pal_bytes ? in.take(packed_bytes(bits)) : nullptr;
                if (!data) return fail("a section that is not one");
                std::memcpy(pal, pal_bytes, 2u * n);
                if (!s.load(out.alloc, bits, pal, n, data)) return fail("a section that is not one");
            }
            c.biome[si] = in.num<u16>();
            const u8 count = in.num<u8>();
            if (count) {
                const u8* pal = count >= 2 && count <= 64 ? in.take(2u * count) : nullptr;
                const u8* index = pal ? in.take(64) : nullptr;
                if (!index) return fail("biomes that are not biomes");
                if (!c.biomes) c.biomes = new (out.arena.alloc(sizeof(BiomeSection) * kMaxSections)) BiomeSection[kMaxSections];
                BiomeSection& b = c.biomes[si];
                b.count = count;
                std::memcpy(b.palette, pal, 2u * count);
                std::memcpy(b.index, index, 64);
                for (u32 i = 0; i < 64; ++i)
                    if (b.index[i] >= count) return fail("biomes that are not biomes");
                for (u32 i = 0; i < count; ++i)
                    if (b.palette[i] >= biome_count()) return fail("a biome that does not exist");
            } else if (c.biome[si] >= biome_count()) {
                return fail("a biome that does not exist");
            }
        }
        for (u32 li = 0; li < l.sections + 2; ++li)
            if (!get_layer(in, out.arena, c.sky[li]) || !get_layer(in, out.arena, c.block[li]))
                return fail("light that is not light");
    }
    if (!blob.empty()) {
        out.parts.block_entities = copy_into(out.arena, in.bytes());
        out.parts.entities = copy_into(out.arena, in.bytes());
        const std::span<const u8> keep = copy_into(out.arena, in.bytes());
        c.keep = {static_cast<u32>(keep.size()), const_cast<u8*>(keep.data())};
        if (!in.ok) return fail("a chunk that ends too early");
    }
    out.traits = derive(c, l);
    return true;
}

}  // namespace world
