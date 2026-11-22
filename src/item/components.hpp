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
// A patch comes from a client (set_creative_mode_slot), from a tag (item/codec.hpp: a command's
// `stone[...]`, a saved stack) or from the server itself (PatchBuilder), and what a client sends
// is shown to other clients: as equipment, as a dropped item. A value another client's decoder
// throws on would disconnect everyone in sight of whoever holds it. So nothing is stored that
// has not been read by a decoder of its own, written from the component's vanilla codecs
// (StreamCodec for the layout, Codec for the value ranges) - and what is stored is that
// decoder's re-encoding, never the client's bytes. Components without such a decoder make the
// whole stack kUnsupported: the server refuses it rather than hold a stack it cannot vouch for
// (the same rule as for blocks it cannot simulate). The list is in components.cpp.
//
// A patch says only what differs from its item: what makes a stack out of one (read_untrusted,
// stack_from_nbt) drops an entry that sets a component to the item's own value, as
// PatchedDataComponentMap does (item::sanitized).
//
// Where this differs from vanilla, knowingly:
//   * patches are capped at kMaxPatchBytes (1 MiB); vanilla's only limit is the packet size.
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

// Largest patch a stack may carry, in wire bytes: enough for a book of a hundred full pages or
// a shulker box of such books, and half of what one frame can hold - so that any one stack can
// be sent. Everything the creative inventory offers is under 100 bytes.
inline constexpr u32 kMaxPatchBytes = 1u << 20;
// A stack whose patch is longer than this is heavy: it is copied to everyone who sees its
// holder, kept in a player's file, sent with every menu that shows it - and what one client can
// make others pay is bounded where it is paid (docs/architecture.md, "A heavy stack"): how fast
// a client may send such stacks, how much of them one player may bring into being, how many
// bytes of them a tick may copy to those who look.
inline constexpr u32 kHeavyPatchBytes = 4096;

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

    // What a patch is put together from: per component type nothing, a value, or its removal.
    struct Slot {
        u8 state = 0;          // 0 absent, 1 set, 2 removed
        u32 off = 0, len = 0;  // the value, in the bytes handed along
    };
    // The patch of these slots (0 for an empty one); the caller owns one reference.
    // kUnsupported: it is larger than a patch may be.
    Verdict assemble(const Slot* slots, std::span<const u8> values, PatchId& out);

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

// One value of component `type` as a client sent it (the bytes its StreamCodec reads), read
// and appended as vanilla would write the value it decoded - the decoders of components.cpp, the
// one place that knows what a value may be. kUnsupported: a component, or a value, that is not read.
Verdict check_value(u32 type, std::span<const u8> value, base::ByteBuf& out);

// Registry.get(TagKey): does the registry ("minecraft:item") have this tag ("minecraft:logs")?
// The tags are those every client is told (update_tags).
bool registry_has_tag(std::string_view registry, std::string_view tag);

// A stack inside a component (ItemStackTemplate), where it lies in the component's value.
struct Contained {
    u32 slot;  // its place in the list (a container's slot)
    u32 item, count;
    std::span<const u8> patch;  // DataComponentPatch.STREAM_CODEC: values without lengths
};
// The stacks a `container`, `bundle_contents` or `charged_projectiles` value holds; a
// container's empty slots are passed over.
void contained(u32 type, std::span<const u8> value, std::vector<Contained>& out);
// One component of such a patch.
Patches::Found find_nested(std::span<const u8> patch, u32 type);

// A patch the server makes: another patch with components set, removed, or put back to the
// item's own. The values are in the form check_value() writes. Tick thread, a cold path.
class PatchBuilder {
  public:
    explicit PatchBuilder(PatchId base = 0);
    void set(u32 type, std::span<const u8> value);
    void remove(u32 type);  // "!type": the item's default is taken away
    void erase(u32 type);   // neither: the item's default stands
    bool empty() const;
    Verdict build(PatchId& out);  // as Patches::assemble

  private:
    Patches::Slot slots_[component::kCount];
    base::ByteBuf values_;
};

}  // namespace item
