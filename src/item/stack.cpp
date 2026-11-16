#include "item/stack.hpp"

namespace item {

Verdict read_untrusted(proto::Reader& r, ItemStack& out) {
    out = {};
    const i32 count = static_cast<i32>(r.varint());
    if (!r.ok) return Verdict::kMalformed;
    if (count <= 0) return Verdict::kOk;  // nothing follows an empty stack
    const u32 item = r.varint();          // Item.STREAM_CODEC: Registry.byIdOrThrow
    if (!r.ok || item >= world::item_count()) return Verdict::kMalformed;
    PatchId patch = 0;
    if (const Verdict v = patches().read_untrusted(r, patch); v != Verdict::kOk) return v;
    // The stack owns the patch from here: every way out below gives the reference back.
    ItemStack stack(item, std::min<u32>(static_cast<u32>(count), kMaxCount), patch);
    if (item == 0) return Verdict::kOk;  // air is an empty stack whatever else it says, and is not validated
    // validatedStreamCodec encodes what it read with ItemStack.CODEC, whose count is intRange(1, 99).
    if (static_cast<u32>(count) > kMaxCount) return Verdict::kMalformed;
    out = std::move(stack);
    return Verdict::kOk;
}

void write_untrusted(base::ByteBuf& out, const ItemStack& s) {
    if (s.empty()) {
        out.commit(proto::put_varint(out.reserve(proto::kVarIntMaxBytes), 0));
        return;
    }
    u8* p = out.reserve(4 * proto::kVarIntMaxBytes);
    p = proto::put_varint(p, s.count());
    p = proto::put_varint(p, s.item());
    if (!s.patch()) {
        p = proto::put_varint(p, 0);
        out.commit(proto::put_varint(p, 0));
        return;
    }
    const std::span<const Patches::Entry> entries = patches().entries(s.patch());
    const std::span<const u8> wire = patches().wire(s.patch());
    u32 removed = 0;
    for (const Patches::Entry& e : entries) removed += e.removed;
    p = proto::put_varint(p, static_cast<u32>(entries.size()) - removed);
    out.commit(proto::put_varint(p, removed));
    for (const Patches::Entry& e : entries) {
        if (e.removed) continue;
        p = out.reserve(2 * proto::kVarIntMaxBytes + e.len);
        p = proto::put_varint(p, e.type);
        p = proto::put_varint(p, e.len);
        std::memcpy(p, wire.data() + e.off, e.len);
        out.commit(p + e.len);
    }
    for (const Patches::Entry& e : entries)
        if (e.removed) out.commit(proto::put_varint(out.reserve(proto::kVarIntMaxBytes), e.type));
}

}  // namespace item
