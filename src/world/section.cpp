// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/section.hpp"

#include <immintrin.h>

namespace world {

// account() adds flag bits as integers.
static_assert(state::kIsAir == 1);

SectionAllocator::SectionAllocator() {
    for (u32 b = 1; b <= Section::kMaxPaletteBits; ++b) pools_[b - 1].init(Section::block_bytes(b));
    pools_[8].init(Section::block_bytes(Section::kDirectBits));
}

u32 SectionAllocator::block_size(u32 bits) { return Section::block_bytes(bits); }

usize SectionAllocator::reserved_bytes() const {
    usize n = 0;
    for (const BlockPool& p : pools_) n += p.reserved_bytes();
    return n;
}

usize SectionAllocator::live_blocks() const {
    usize n = 0;
    for (const BlockPool& p : pools_) n += p.live();
    return n;
}

namespace {

// A palette's slot of state s: Section::find(), and a palette that is still being built on the stack.
ALWAYS_INLINE u32 find_in(const u16* pal, u32 n, u32 s) {
    const __m256i key = _mm256_set1_epi16(static_cast<short>(s));
    for (u32 i = 0; i < n; i += 16) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pal + i));
        const u32 m = static_cast<u32>(_mm256_movemask_epi8(_mm256_cmpeq_epi16(v, key)));
        if (m) return i + (static_cast<u32>(std::countr_zero(m)) >> 1);
    }
    return Section::kNoSlot;
}

}  // namespace

u32 Section::find(BlockState s) const { return find_in(palette, palette_len, s); }

namespace {

ALWAYS_INLINE u32 air_bit(BlockState s) { return flags_of(s) & state::kIsAir; }
ALWAYS_INLINE u32 fluid_bit(BlockState s) { return (flags_of(s) & state::kHasFluid) != 0; }
ALWAYS_INLINE u32 tick_bit(BlockState s) { return state::kRandom[s] & state::kRandomTick; }
static_assert(state::kRandomTick == 1);  // (counted by adding the bit)

}  // namespace

void Section::attach(SectionAllocator& a, u32 new_bits) {
    u8* block = static_cast<u8*>(a.alloc(new_bits));
    bits = static_cast<u8>(new_bits);
    if (new_bits == kDirectBits) {
        data = block;
        palette = nullptr;
        return;
    }
    palette = reinterpret_cast<u16*>(block);
    data = block + 2 * palette_cap(new_bits);
    // The bulk kernels read (and pack_indices writes) a few bytes past the last entry.
    std::memset(data + packed_bytes(new_bits), 0, kPackedSlack);
}

void Section::detach(SectionAllocator& a) {
    if (bits) a.free(bits, bits == kDirectBits ? static_cast<void*>(data) : static_cast<void*>(palette));
    data = nullptr;
    palette = nullptr;
    bits = 0;
    palette_len = 0;
    dead = 0;
}

void Section::release(SectionAllocator& a) {
    detach(a);
    single = kAir;
    non_air = fluids = ticking = 0;
}

void Section::fill(SectionAllocator& a, BlockState s) {
    detach(a);
    single = s;
    recount();
}

void Section::recount() {
    u32 solid = 0, wet = 0, ticks = 0;
    if (bits == 0) {
        solid = air_bit(single) ? 0 : kVolume;
        wet = fluid_bit(single) ? kVolume : 0;
        ticks = tick_bit(single) ? kVolume : 0;
    } else if (bits == kDirectBits) {
        for (u32 i = 0; i < kVolume; ++i) {
            const BlockState s = load_le<u16>(data + 2 * i);
            solid += 1 - air_bit(s);
            wet += fluid_bit(s);
            ticks += tick_bit(s);
        }
    } else {
        const u16* r = refs();
        for (u32 i = 0; i < palette_len; ++i) {
            if (!r[i]) continue;
            solid += air_bit(palette[i]) ? 0 : r[i];
            wet += fluid_bit(palette[i]) ? r[i] : 0;
            ticks += tick_bit(palette[i]) ? r[i] : 0;
        }
    }
    non_air = static_cast<u16>(solid);
    fluids = static_cast<u16>(wet);
    ticking = static_cast<u16>(ticks);
}

BlockState Section::set(SectionAllocator& a, u32 i, BlockState s) {
    DCHECK(i < kVolume && s < state::kCount);
    if (bits == 0) {
        if (s == single) return s;
        // Second state in a uniform section: start a 1-bit palette with every index 0.
        const BlockState only = single;
        attach(a, 1);
        const u32 cap = palette_cap(1);
        std::fill_n(palette, cap, kNoState);
        std::memset(data, 0, packed_bytes(1));
        u16* r = refs();
        std::fill_n(r, cap, u16{0});
        palette[0] = only;
        r[0] = kVolume;
        palette_len = 1;
    }
    if (UNLIKELY(bits == kDirectBits)) {
        const BlockState old = load_le<u16>(data + 2 * i);
        if (old == s) return s;
        store_le<u16>(data + 2 * i, s);
        non_air = static_cast<u16>(non_air + air_bit(old) - air_bit(s));
        fluids = static_cast<u16>(fluids + fluid_bit(s) - fluid_bit(old));
        ticking = static_cast<u16>(ticking + tick_bit(s) - tick_bit(old));
        return old;
    }
    u32 slot = find(s);
    if (UNLIKELY(slot == kNoSlot)) {
        slot = add_slot(a, s);
        if (slot == kNoSlot) return set(a, i, s);  // the 257th state: now in direct storage
    }
    const u32 old_slot = packed_exchange(data, bits, i, slot);
    if (old_slot == slot) return s;
    u16* r = refs();
    const BlockState old = palette[old_slot];
    dead = static_cast<u16>(dead + (--r[old_slot] == 0));
    dead = static_cast<u16>(dead - (r[slot]++ == 0));
    non_air = static_cast<u16>(non_air + air_bit(old) - air_bit(s));
    fluids = static_cast<u16>(fluids + fluid_bit(s) - fluid_bit(old));
    ticking = static_cast<u16>(ticking + tick_bit(s) - tick_bit(old));
    if (UNLIKELY(r[slot] == kVolume)) {  // one state everywhere again: give the block back
        detach(a);
        single = s;
    }
    return old;
}

u32 Section::add_slot(SectionAllocator& a, BlockState s) {
    u16* r = refs();
    if (dead) {
        for (u32 i = 0; i < palette_len; ++i) {
            if (r[i] == 0) {
                palette[i] = s;
                return i;  // still counted in `dead` until the caller gives it a block
            }
        }
    }
    if (palette_len == (1u << bits)) {
        if (bits == kMaxPaletteBits) {
            to_direct(a);
            return kNoSlot;
        }
        grow(a);
        r = refs();
    }
    const u32 slot = palette_len++;
    palette[slot] = s;
    r[slot] = 0;
    ++dead;
    return slot;
}

void Section::grow(SectionAllocator& a) {
    alignas(64) u8 idx[kVolume];
    unpack_indices(data, bits, idx);
    const u32 old_bits = bits, n = palette_len;
    u16* old_pal = palette;
    u16* old_refs = refs();
    attach(a, old_bits + 1);
    const u32 cap = palette_cap(bits);
    u16* r = refs();
    std::copy_n(old_pal, n, palette);
    std::fill(palette + n, palette + cap, kNoState);
    std::copy_n(old_refs, n, r);
    std::fill(r + n, r + cap, u16{0});
    pack_indices(idx, bits, data);
    a.free(old_bits, old_pal);
}

void Section::to_direct(SectionAllocator& a) {
    alignas(64) u8 idx[kVolume];
    unpack_indices(data, bits, idx);
    const u32 old_bits = bits;
    u16* old_pal = palette;
    attach(a, kDirectBits);
    for (u32 i = 0; i < kVolume; ++i) store_le<u16>(data + 2 * i, old_pal[idx[i]]);
    a.free(old_bits, old_pal);
    palette_len = 0;
    dead = 0;
}

void Section::assign(SectionAllocator& a, const BlockState* states) {
    alignas(32) u16 pal[256];
    u16 cnt[256];
    alignas(64) u8 idx[kVolume];
    std::fill_n(pal, 256, kNoState);
    std::memset(cnt, 0, sizeof cnt);
    u32 n = 0;
    // Generators and loaders hand over long runs of one state: remember the last lookup.
    u32 last = kNoState, last_slot = 0;
    bool direct = false;
    for (u32 i = 0; i < kVolume; ++i) {
        const u32 s = states[i];
        DCHECK(s < state::kCount);
        if (s != last) {
            u32 slot = find_in(pal, n, s);
            if (slot == kNoSlot) {
                if (n == 256) {
                    direct = true;
                    break;
                }
                pal[n] = static_cast<u16>(s);
                slot = n++;
            }
            last = s;
            last_slot = slot;
        }
        idx[i] = static_cast<u8>(last_slot);
        ++cnt[last_slot];
    }
    detach(a);
    if (direct) {
        attach(a, kDirectBits);
        std::memcpy(data, states, kVolume * 2);
    } else if (n == 1) {
        single = pal[0];
    } else {
        attach(a, bits_for(n));
        const u32 cap = palette_cap(bits);
        std::copy_n(pal, cap, palette);
        std::copy_n(cnt, cap, refs());
        pack_indices(idx, bits, data);
        palette_len = static_cast<u16>(n);
    }
    recount();
}

void Section::copy_from(SectionAllocator& a, const Section& o) {
    detach(a);
    single = o.single;
    non_air = o.non_air;
    fluids = o.fluids;
    ticking = o.ticking;
    if (o.bits == 0) return;
    attach(a, o.bits);
    const void* src = o.bits == kDirectBits ? static_cast<const void*>(o.data) : static_cast<const void*>(o.palette);
    std::memcpy(bits == kDirectBits ? static_cast<void*>(data) : static_cast<void*>(palette), src, block_bytes(bits));
    palette_len = o.palette_len;
    dead = o.dead;
}

bool Section::load(SectionAllocator& a, u32 new_bits, const u16* pal, u32 pal_len, const u8* packed) {
    if (new_bits == kDirectBits) {
        for (u32 i = 0; i < kVolume; ++i)
            if (load_le<u16>(packed + 2 * i) >= state::kCount) return false;
        detach(a);
        attach(a, kDirectBits);
        std::memcpy(data, packed, kVolume * 2);
        recount();
        return true;
    }
    if (new_bits < 1 || new_bits > kMaxPaletteBits || pal_len < 1 || pal_len > (1u << new_bits)) return false;
    for (u32 i = 0; i < pal_len; ++i)
        if (pal[i] >= state::kCount) return false;
    alignas(64) u8 idx[kVolume];
    u16 cnt[256] = {};
    {
        // A copy with slack behind it: the kernel reads whole words, `packed` may end at its last byte.
        alignas(64) u8 dense[kVolume + kPackedSlack];
        std::memcpy(dense, packed, packed_bytes(new_bits));
        std::memset(dense + packed_bytes(new_bits), 0, kPackedSlack);
        unpack_indices(dense, new_bits, idx);
    }
    for (u32 i = 0; i < kVolume; ++i) {
        if (idx[i] >= pal_len) return false;
        ++cnt[idx[i]];
    }
    detach(a);
    attach(a, new_bits);
    const u32 cap = palette_cap(bits);
    std::fill_n(palette, cap, kNoState);
    std::copy_n(pal, pal_len, palette);
    std::memcpy(data, packed, packed_bytes(bits));
    u16* r = refs();
    std::fill_n(r, cap, u16{0});
    u32 none = 0;
    for (u32 i = 0; i < pal_len; ++i) {
        r[i] = cnt[i];
        none += cnt[i] == 0;
    }
    palette_len = static_cast<u16>(pal_len);
    dead = static_cast<u16>(none);
    recount();
    return true;
}

void Section::unpack(BlockState* RESTRICT out) const {
    if (bits == 0) {
        std::fill_n(out, kVolume, single);
    } else if (bits == kDirectBits) {
        std::memcpy(out, data, kVolume * 2);
    } else {
        alignas(64) u8 idx[kVolume];
        unpack_indices(data, bits, idx);
        const u16* pal = palette;
        for (u32 i = 0; i < kVolume; ++i) out[i] = pal[idx[i]];
    }
}

void Section::compact(SectionAllocator& a) {
    if (bits == 0) return;
    if (bits == kDirectBits) {
        // No reference counts here: count distinct states, giving up as soon as a palette cannot fit.
        u64 seen[1024] = {};
        u32 distinct = 0;
        for (u32 i = 0; i < kVolume; ++i) {
            const u32 s = load_le<u16>(data + 2 * i);
            u64& w = seen[s >> 6];
            const u64 b = 1ull << (s & 63);
            if (!(w & b)) {
                w |= b;
                if (++distinct > 256) return;
            }
        }
        BlockState states[kVolume];
        std::memcpy(states, data, sizeof states);
        assign(a, states);
        return;
    }
    if (!dead) return;
    const u32 live = palette_len - dead;
    if (bits_for(std::max<u32>(live, 2)) == bits) return;  // dead slots that cost no bit stay for reuse
    u8 remap[256];
    alignas(32) u16 pal[256];
    u16 cnt[256];
    std::fill_n(pal, 256, kNoState);
    std::memset(cnt, 0, sizeof cnt);
    u32 n = 0;
    const u16* r = refs();
    for (u32 i = 0; i < palette_len; ++i) {
        remap[i] = static_cast<u8>(n);
        if (!r[i]) continue;
        pal[n] = palette[i];
        cnt[n] = r[i];
        ++n;
    }
    alignas(64) u8 idx[kVolume];
    unpack_indices(data, bits, idx);
    for (u32 i = 0; i < kVolume; ++i) idx[i] = remap[idx[i]];
    detach(a);
    if (n == 1) {
        single = pal[0];
        return;
    }
    attach(a, bits_for(n));
    const u32 cap = palette_cap(bits);
    std::copy_n(pal, cap, palette);
    std::copy_n(cnt, cap, refs());
    pack_indices(idx, bits, data);
    palette_len = static_cast<u16>(n);
}

}  // namespace world
