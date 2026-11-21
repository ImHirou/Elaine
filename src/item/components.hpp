// SPDX-License-Identifier: GPL-3.0-or-later
// Component patches: the part of an item stack that is not its item and its count.
//
// In vanilla a stack carries a DataComponentPatch - "these components are set to these values,
// those are removed" - on top of its item's default components. Here every distinct patch is
// stored once, as the bytes DataComponentPatch.STREAM_CODEC writes for it, and a stack holds a
// 32-bit id (item::ItemStack). What that buys:
//   * a stack is 8 bytes whatever it carries, and almost every stack carries nothing (id 0);
//   * "same item, same components" - the test behind every merge - is one integer compare;
//   * sending a stack to a client copies bytes that are already in wire form.
//
// A patch only ever comes from a client (set_creative_mode_slot), and what a client sends is
// shown to other clients: as equipment, as a dropped item. A value another client's decoder
// throws on would disconnect everyone in sight of whoever holds it. So nothing is stored that
// has not been read by a decoder of its own, written from the component's vanilla codecs
// (StreamCodec for the layout, Codec for the value ranges) - and what is stored is that
// decoder's re-encoding, never the client's bytes. Components without such a decoder make the
// whole stack kUnsupported: the server refuses it rather than hold a stack it cannot vouch for
// (the same rule as for blocks it cannot simulate). The list is in components.cpp.
//
// Where this differs from vanilla, knowingly:
//   * a patch is not "sanitized" against the item's defaults (PatchedDataComponentMap drops an
//     entry that sets a component to its default value). A vanilla client never sends such an
//     entry; from a modified one it makes a stack that does not merge with its plain twin;
//   * patches are capped at kMaxPatchBytes; vanilla's only limit is the packet size.
//
// Tick thread only: ids are reference-counted without atomics.
#pragma once

#include <span>
#include <vector>

#include "base/buf.hpp"
#include "base/types.hpp"
#include "proto/gen/current.hpp"
#include "proto/wire.hpp"

#include PROTO_GEN_ITEMS_HPP

namespace item {

namespace component = proto::gen::component;

using PatchId = u32;  // 0 = no patch

// Largest patch a stack may carry, in wire bytes. A stack is copied to everyone who sees its
// holder; without a cap one client decides how many megabytes that is. Everything the creative
// inventory offers is under 100 bytes.
inline constexpr u32 kMaxPatchBytes = 1024;

enum class Verdict : u8 {
    kOk,
    kUnsupported,  // may be fine for vanilla, but carries something this server does not check yet
    kMalformed,    // vanilla's decoder throws on these bytes: the sender gets disconnected
};

class Patches {
  public:
    // One component of a patch.
    struct Entry {
        u16 type;
        bool removed;  // "!type": the item's default is taken away; no value
        u32 off, len;  // the value, inside the patch's wire bytes
    };
    enum class State : u8 { kInherit, kSet, kRemoved };
    struct Found {
        State state = State::kInherit;
        std::span<const u8> value;  // kSet: as the component's StreamCodec writes it
    };

    Patches();
    ~Patches();
    Patches(const Patches&) = delete;
    Patches& operator=(const Patches&) = delete;

    // DataComponentPatch.DELIMITED_STREAM_CODEC - a patch as a client sends it, every value
    // length-prefixed - from `r`. kOk: `out` is the patch (0 for an empty one) and the caller
    // owns one reference. Otherwise `out` is 0; after kUnsupported the reader stands behind the
    // patch, as it does after kOk.
    Verdict read_untrusted(proto::Reader& r, PatchId& out);

    ALWAYS_INLINE void retain(PatchId id) { ++slots_[id]->refs; }
    ALWAYS_INLINE void release(PatchId id) {
        if (UNLIKELY(--slots_[id]->refs == 0)) drop(id);
    }

    // The patch as DataComponentPatch.STREAM_CODEC writes it: counts, added entries, removed types.
    std::span<const u8> wire(PatchId id) const { return {slots_[id]->wire(), slots_[id]->wire_len}; }
    std::span<const Entry> entries(PatchId id) const { return {slots_[id]->index(), slots_[id]->entries}; }
    Found find(PatchId id, u32 type) const;

    u32 live() const { return live_; }  // distinct patches held by at least one stack

  private:
    struct Patch {  // followed by Entry[entries] (sorted by type), then u8[wire_len]
        u32 refs;
        u32 hash;
        u32 wire_len;
        u32 entries;
        const Entry* index() const { return reinterpret_cast<const Entry*>(this + 1); }
        Entry* index() { return reinterpret_cast<Entry*>(this + 1); }
        const u8* wire() const { return reinterpret_cast<const u8*>(index() + entries); }
        u8* wire() { return reinterpret_cast<u8*>(index() + entries); }
    };

    PatchId intern(std::span<const Entry> index, std::span<const u8> wire);
    void drop(PatchId id);
    void rehash(u32 buckets);

    std::vector<Patch*> slots_;   // id -> patch; [0] is never used
    std::vector<PatchId> free_;   // ids to hand out again
    std::vector<PatchId> table_;  // open addressing by Patch::hash, 0 = empty; a power of two, at most half full
    u32 live_ = 0;
    base::ByteBuf values_;  // read_untrusted: the re-encoded values of the patch being read
    base::ByteBuf wire_;    // ...and the patch being assembled from them
};

// The store of the tick thread (and of a test's only thread).
Patches& patches();

}  // namespace item
