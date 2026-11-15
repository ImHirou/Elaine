// NBT and text component benchmarks: what a tag costs to read, to write back and to build, and
// what a stack costs to read once it carries text.
#include <vector>

#include "bench.hpp"

#include "item/stack.hpp"
#include "nbt/nbt.hpp"
#include "text/component.hpp"

#include PROTO_GEN_ITEMS_HPP

namespace benchmark {

namespace {

namespace it = proto::gen::item;
namespace comp = proto::gen::component;

// A chest as it is saved: its position, a name, 27 stacks - a few with components of their own.
void chest(nbt::Writer& w) {
    w.root(nbt::kCompound);
    w.put_string("id", "minecraft:chest");
    w.put_int("x", -1204);
    w.put_int("y", 64);
    w.put_int("z", 388);
    w.put_string("CustomName", "Loot");
    w.begin_list("Items", nbt::kCompound, 27);
    for (int slot = 0; slot < 27; ++slot) {
        w.begin_compound();
        w.put_byte("Slot", static_cast<i8>(slot));
        w.put_string("id", slot % 3 ? "minecraft:cobblestone" : "minecraft:diamond_sword");
        w.put_int("count", slot % 3 ? 64 : 1);
        if (slot % 3 == 0) {
            w.begin_compound("components");
            w.put_int("minecraft:damage", 17 * slot);
            w.begin_compound("minecraft:enchantments");
            w.put_int("minecraft:sharpness", 5);
            w.put_int("minecraft:unbreaking", 3);
            w.end_compound();
            w.end_compound();
        }
        w.end_compound();
    }
    w.end_compound();
}

// A stack with these components, as set_creative_mode_slot carries it.
std::vector<u8> untrusted(u32 item_id, std::initializer_list<std::pair<u32, base::ByteBuf*>> components) {
    base::ByteBuf b;
    const auto varint = [&](u32 v) { b.commit(proto::put_varint(b.reserve(5), v)); };
    varint(1);
    varint(item_id);
    varint(static_cast<u32>(components.size()));
    varint(0);
    for (const auto& [type, value] : components) {
        varint(type);
        varint(value->len);
        b.append(value->data, value->len);
    }
    return {b.data, b.data + b.len};
}

}  // namespace

void nbt_benchmarks() {
    {
        base::ByteBuf tag, out;
        nbt::Writer w(tag);
        chest(w);
        nbt::Doc doc;
        const std::span<const u8> bytes(tag.data, tag.len);
        if (doc.parse(bytes) != nbt::Error::kNone) std::abort();
        if (selected("nbt:"))
            std::printf("%-44s %12u bytes, %u bytes on the tape\n", "  (the chest below)", tag.len,
                        static_cast<u32>(doc.root().doc->node(0).next * sizeof(nbt::Node)));
        bench("nbt: read a chest of 27 stacks", static_cast<double>(tag.len), "MB/s", [&] {
            doc.parse(bytes);
            g_sink = doc.size();
        });
        bench("nbt: find an entry of it by name", 1, "M/s", [&] { g_sink = static_cast<u64>(doc.root().get("Items").size()); });
        bench("nbt: write it back, names in order", static_cast<double>(tag.len), "MB/s", [&] {
            out.clear();
            doc.write(doc.root(), out);
            g_sink = out.len;
        });
        bench("nbt: build it with the Writer", static_cast<double>(tag.len), "MB/s", [&] {
            out.clear();
            nbt::Writer again(out);
            chest(again);
            g_sink = out.len;
        });
    }
    {
        // A name as an anvil makes it, and one as /give makes it: a record with a style, a child,
        // a hover text - spelled with its keys in the order a person writes them.
        base::ByteBuf plain, styled, lore, out;
        nbt::Writer(plain).root(nbt::kString);
        nbt::Writer(plain).put_string("Excalibur");
        nbt::Writer s(styled);
        s.root(nbt::kCompound);
        s.put_string("text", "Excalibur");
        s.put_string("color", "gold");
        s.put_byte("italic", 0);
        s.put_byte("bold", 1);
        s.begin_compound("hover_event");
        s.put_string("action", "show_text");
        s.put_string("value", "Forged in fire");
        s.end_compound();
        s.begin_list("extra", nbt::kCompound, 1);
        s.put_string("text", " +5");
        s.put_string("color", "aqua");
        s.end_compound();
        s.end_compound();
        nbt::Doc doc;
        if (doc.parse({styled.data, styled.len}) != nbt::Error::kNone) std::abort();
        bench("text: a styled name through the codec", 1, "M/s", [&] {
            out.clear();
            doc.parse({styled.data, styled.len});
            text::transcode(doc.root(), out);
            g_sink = out.len;
        });

        lore.commit(proto::put_varint(lore.reserve(5), 2));
        lore.append(plain.data, plain.len);
        lore.append(styled.data, styled.len);
        const std::vector<u8> named = untrusted(it::kDiamondSword, {{comp::kCustomName, &plain}});
        const std::vector<u8> full = untrusted(it::kDiamondSword, {{comp::kCustomName, &styled}, {comp::kLore, &lore}});
        item::ItemStack held[2];
        for (const auto* stack : {&named, &full}) {  // kept alive: the reads below find their patches interned
            proto::Reader r(stack->data(), stack->size());
            if (item::read_untrusted(r, held[stack == &full]) != item::Verdict::kOk) std::abort();
        }
        bench("item: read a stack with a plain name", 1, "M/s", [&] {
            proto::Reader r(named.data(), named.size());
            item::ItemStack stack;
            item::read_untrusted(r, stack);
            g_sink = stack.patch();
        });
        bench("item: read a stack with a styled name + lore", 1, "M/s", [&] {
            proto::Reader r(full.data(), full.size());
            item::ItemStack stack;
            item::read_untrusted(r, stack);
            g_sink = stack.patch();
        });
    }
}

}  // namespace benchmark
