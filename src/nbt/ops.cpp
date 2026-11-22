// SPDX-License-Identifier: GPL-3.0-or-later
#include "nbt/ops.hpp"

#include <algorithm>
#include <cstring>

namespace nbt {

namespace {

bool same_bytes(Tag a, Tag b) {
    const auto x = a.bytes(), y = b.bytes();
    return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size()) == 0;
}

// How many names a compound holds: an entry that comes twice is one (a Doc of a client's bytes
// may hold such; what the server writes does not).
u32 distinct(Tag compound) {
    u32 n = 0;
    for (const Tag e : compound.entries()) n += compound.get(e.name()).at == e.at;
    return n;
}

}  // namespace

bool equal(Tag a, Tag b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case kEnd: return true;
        case kFloat: {
            const float x = static_cast<float>(a.as_double()), y = static_cast<float>(b.as_double());
            return (x != x && y != y) || same_bytes(a, b);
        }
        case kDouble: {
            const double x = a.as_double(), y = b.as_double();
            return (x != x && y != y) || same_bytes(a, b);
        }
        case kList: {
            if (a.size() != b.size()) return false;
            auto ea = a.elements(), eb = b.elements();
            auto ia = ea.begin(), ib = eb.begin();
            for (; ia != ea.end(); ++ia, ++ib)
                if (!equal(*ia, *ib)) return false;
            return true;
        }
        case kCompound: {
            if (distinct(a) != distinct(b)) return false;
            for (const Tag e : a.entries()) {
                const Tag mine = a.get(e.name());
                if (mine.at != e.at) continue;  // shadowed by a later entry of the same name
                const Tag other = b.get(e.name());
                if (!other || !equal(mine, other)) return false;
            }
            return true;
        }
        default: return same_bytes(a, b);  // numbers, strings, arrays
    }
}

bool matches(Tag expected, Tag actual, bool partial_lists) {
    if (!expected) return true;
    if (!actual || expected.type != actual.type) return false;
    if (expected.type == kCompound) {
        if (distinct(actual) < distinct(expected)) return false;
        for (const Tag e : expected.entries()) {
            const Tag asked = expected.get(e.name());
            if (asked.at != e.at) continue;
            if (!matches(asked, actual.get(e.name()), partial_lists)) return false;
        }
        return true;
    }
    if (expected.type == kList && partial_lists) {
        if (expected.size() == 0) return actual.size() == 0;
        if (actual.size() < expected.size()) return false;
        for (const Tag asked : expected.elements()) {
            bool found = false;
            for (const Tag have : actual.elements()) {
                if (matches(asked, have, partial_lists)) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }
    return equal(expected, actual);
}

void merge(Tag into, Tag from, base::ByteBuf& out) {
    Writer w(out);
    for (const Tag e : into.entries()) {
        if (into.get(e.name()).at != e.at) continue;
        const Tag over = from.get(e.name());
        if (!over) {
            w.put_raw(e.name(), e.type, e.bytes());
        } else if (over.type == kCompound && e.type == kCompound) {
            w.entry(kCompound, e.name());
            merge(e, over, out);
        } else {
            w.put_raw(e.name(), over.type, over.bytes());
        }
    }
    for (const Tag e : from.entries()) {
        if (from.get(e.name()).at != e.at || into.get(e.name())) continue;
        w.put_raw(e.name(), e.type, e.bytes());
    }
    w.end_compound();
}

u32 utf16_length(std::string_view mutf8) {
    u32 n = 0;
    for (const char c : mutf8) n += (static_cast<u8>(c) & 0xC0) != 0x80;  // a surrogate is a sequence of its own
    return n;
}

void map_order(Tag compound, std::vector<Tag>& out) {
    struct Slot {
        u32 hash;
        Tag tag;
    };
    std::vector<Slot> slots;
    for (const Tag e : compound.entries()) {
        const std::string_view name = e.name();
        const Tag kept = compound.get(name);
        bool seen = false;  // put() of a name that is there keeps its place and takes the value
        for (const Slot& s : slots) seen |= s.tag.at == kept.at;
        if (seen) continue;
        u32 h = 0;  // String.hashCode: over UTF-16 units, each of which is one sequence here
        const u8* p = reinterpret_cast<const u8*>(name.data());
        const u8* const end = p + name.size();
        while (p < end) {
            u32 unit = *p++;
            if (unit >= 0xE0 && end - p >= 2) {
                unit = (unit & 0x0F) << 12 | (p[0] & 0x3Fu) << 6 | (p[1] & 0x3Fu);
                p += 2;
            } else if (unit >= 0xC0 && end - p >= 1) {
                unit = (unit & 0x1F) << 6 | (p[0] & 0x3Fu);
                p += 1;
            }
            h = 31 * h + unit;
        }
        slots.push_back({h ^ (h >> 16), kept});  // HashMap.hash
    }
    // The table starts with 16 buckets and doubles when more than three quarters are in use.
    u32 mask = 15;
    while (slots.size() * 4 > (static_cast<usize>(mask) + 1) * 3) mask = mask * 2 + 1;
    std::stable_sort(slots.begin(), slots.end(),
                     [mask](const Slot& a, const Slot& b) { return (a.hash & mask) < (b.hash & mask); });
    for (const Slot& s : slots) out.push_back(s.tag);
}

void write_map_order(Tag tag, base::ByteBuf& out) {
    Writer w(out);
    if (tag.type == kCompound) {
        std::vector<Tag> entries;
        map_order(tag, entries);
        for (const Tag e : entries) {
            w.entry(e.type, e.name());
            write_map_order(e, out);
        }
        w.end_compound();
    } else if (tag.type == kList) {
        // ListTag.write: the elements' one type, or compounds - what is not one wrapped, and a
        // compound that would itself be read back as a wrapper wrapped too.
        Type of = kEnd;
        for (const Tag e : tag.elements()) {
            if (of == kEnd) {
                of = e.type;
            } else if (of != e.type) {
                of = kCompound;
                break;
            }
            if (e.flat) break;
        }
        w.begin_list(of, tag.size());
        if (tag.size() && kFixedSize[of] && tag.element_type() == of) {  // numbers, as they are stored
            const auto raw = tag.bytes();
            out.append(raw.data() + 5, raw.size() - 5);
            return;
        }
        for (const Tag e : tag.elements()) {
            if (of == kCompound && (e.type != kCompound || e.wrapper())) {
                w.entry(e.type, "");
                write_map_order(e, out);
                w.end_compound();
            } else {
                write_map_order(e, out);
            }
        }
    } else {
        const auto raw = tag.bytes();
        out.append(raw.data(), raw.size());
    }
}

// ---------------------------------------------------------------------------- paths

namespace {

// Node.getTag: what the node names under `parent`.
void step(const Path::Node& node, Tag pattern, Tag parent, std::vector<Tag>& out) {
    switch (node.kind) {
        case Path::Node::kChild:
            if (const Tag child = parent.get(node.name)) out.push_back(child);
            break;
        case Path::Node::kMatchObject:
            if (parent.type == kCompound) {
                const Tag child = parent.get(node.name);
                if (matches(pattern, child, true)) out.push_back(child);
            }
            break;
        case Path::Node::kMatchRoot:
            if (parent.type == kCompound && matches(pattern, parent, true)) out.push_back(parent);
            break;
        case Path::Node::kIndex: {
            if (!parent.collection()) break;
            const i64 size = parent.size();
            const i64 at = node.index < 0 ? size + node.index : node.index;
            if (at < 0 || at >= size) break;
            i64 i = 0;
            for (const Tag e : parent.elements()) {
                if (i++ == at) {
                    out.push_back(e);
                    break;
                }
            }
            break;
        }
        case Path::Node::kAll:
            if (parent.collection())
                for (const Tag e : parent.elements()) out.push_back(e);
            break;
        case Path::Node::kMatchElement:
            if (parent.type == kList)
                for (const Tag e : parent.elements())
                    if (matches(pattern, e, true)) out.push_back(e);
            break;
    }
}

}  // namespace

bool find(const Path& path, Tag root, std::vector<Tag>& out, std::string_view& missing) {
    std::vector<Tag> found{root}, next;
    Doc pattern_doc;
    for (const Path::Node& node : path.nodes) {
        Tag pattern;
        if (!node.pattern.empty() && pattern_doc.parse(node.pattern, Form::kNameless, kTrusted) == Error::kNone)
            pattern = pattern_doc.root();
        next.clear();
        for (const Tag t : found) step(node, pattern, t, next);
        if (next.empty()) {
            missing = std::string_view(path.text).substr(0, node.end);
            return false;
        }
        found.swap(next);
    }
    out = std::move(found);
    return true;
}

}  // namespace nbt
