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

}  // namespace item
