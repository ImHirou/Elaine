// SPDX-License-Identifier: GPL-3.0-or-later
// The command tree: Brigadier's CommandDispatcher (parse, the choice between readings, the
// usage lines), its StringReader and argument types, and vanilla's Commands.finishParsing and
// ClientboundCommandsPacket around them. Read from brigadier-1.3.11.jar and the 26.3 server jar.
#include "server/command.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <deque>

#include "base/log.hpp"
#include "proto/gen/current.hpp"
#include "proto/identifier.hpp"
#include "proto/wire.hpp"

#include PROTO_GEN_COMMAND_DATA_HPP

namespace server::cmd {

using text::Text;
namespace argument = proto::gen::command::argument;

namespace {

constexpr u32 kMessageMaxChars = 256;  // MessageArgument.Message.parseText: what a message may be long

// ---------------------------------------------------------------------------- StringReader

struct Reader {
    std::string_view s;
    u32 at;
    bool can_read(u32 n = 1) const { return at + n <= s.size(); }
    char peek() const { return s[at]; }
    std::string_view rest() const { return s.substr(at); }
};

Error error_at(const Reader& r, Text message) { return {std::move(message), static_cast<i32>(r.at)}; }

bool allowed_number(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-'; }
bool allowed_unquoted(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.' ||
           c == '+';
}

std::string_view read_number_text(Reader& r) {
    const u32 start = r.at;
    while (r.can_read() && allowed_number(r.peek())) ++r.at;
    return r.s.substr(start, r.at - start);
}
// What Double.parseDouble takes of the characters a number may be written with: an optional
// sign, digits with at most one point among them.
bool plain_decimal(std::string_view t) {
    usize i = t.starts_with('-') ? 1 : 0;
    usize digits = 0, points = 0;
    for (; i < t.size(); ++i) {
        if (t[i] == '.')
            ++points;
        else if (t[i] >= '0' && t[i] <= '9')
            ++digits;
        else
            return false;
    }
    return digits > 0 && points <= 1;
}

// StringReader.readInt
bool read_int(Reader& r, i32& out, Error& err) {
    const u32 start = r.at;
    const std::string_view t = read_number_text(r);
    if (t.empty()) {
        err = error_at(r, Text::translatable("parsing.int.expected"));
        return false;
    }
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), out);  // Integer.parseInt: a sign, digits
    if (ec != std::errc() || end != t.data() + t.size()) {
        r.at = start;
        err = error_at(r, Text::translatable("parsing.int.invalid").arg(t));
        return false;
    }
    return true;
}
// StringReader.readDouble / readFloat
bool read_double(Reader& r, double& out, Error& err, bool as_float = false) {
    const u32 start = r.at;
    const std::string_view t = read_number_text(r);
    if (t.empty()) {
        err = error_at(r, Text::translatable(as_float ? "parsing.float.expected" : "parsing.double.expected"));
        return false;
    }
    if (!plain_decimal(t)) {
        r.at = start;
        err = error_at(r, Text::translatable(as_float ? "parsing.float.invalid" : "parsing.double.invalid").arg(t));
        return false;
    }
    const std::string z(t);
    out = as_float ? static_cast<double>(std::strtof(z.c_str(), nullptr)) : std::strtod(z.c_str(), nullptr);
    return true;
}
std::string_view read_unquoted(Reader& r) {
    const u32 start = r.at;
    while (r.can_read() && allowed_unquoted(r.peek())) ++r.at;
    return r.s.substr(start, r.at - start);
}
// StringReader.readStringUntil
bool read_until(Reader& r, char terminator, std::string& out, Error& err) {
    bool escaped = false;
    while (r.can_read()) {
        const char c = r.s[r.at++];
        if (escaped) {
            if (c != terminator && c != '\\') {
                --r.at;
                err = error_at(r, Text::translatable("parsing.quote.escape").arg(std::string_view(&c, 1)));
                return false;
            }
            out += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == terminator) {
            return true;
        } else {
            out += c;
        }
    }
    err = error_at(r, Text::translatable("parsing.quote.expected.end"));
    return false;
}
// StringReader.readString: a word, or a phrase in either kind of quotes.
bool read_string(Reader& r, std::string& out, Error& err) {
    out.clear();
    if (!r.can_read()) return true;
    const char next = r.peek();
    if (next == '"' || next == '\'') {
        ++r.at;
        return read_until(r, next, out, err);
    }
    out = read_unquoted(r);
    return true;
}

// Identifier.read: as far as the characters of an identifier go.
bool read_identifier(Reader& r, std::string& out, Error& err) {
    const u32 start = r.at;
    const auto allowed = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c == '/' || c == '.' || c == '-';
    };
    while (r.can_read() && allowed(r.peek())) ++r.at;
    proto::Identifier id;
    if (!proto::parse_identifier(r.s.substr(start, r.at - start), id)) {
        r.at = start;
        err = error_at(r, Text::translatable("argument.id.invalid"));
        return false;
    }
    out.assign(id.ns);
    out += ':';
    out += id.path;
    return true;
}

// ---------------------------------------------------------------------------- the syntax line

struct TypeName {
    std::string_view name;
    Type type;
};
constexpr TypeName kTypes[] = {
    {"bool", Type::kBool},
    {"int", Type::kInt},
    {"float", Type::kFloat},
    {"double", Type::kDouble},
    {"word", Type::kWord},
    {"string", Type::kString},
    {"greedy", Type::kGreedy},
    {"entity", Type::kEntity},
    {"entities", Type::kEntities},
    {"player", Type::kPlayer},
    {"players", Type::kPlayers},
    {"profile", Type::kProfile},
    {"block_pos", Type::kBlockPos},
    {"vec3", Type::kVec3},
    {"rotation", Type::kRotation},
    {"block_state", Type::kBlockState},
    {"block_predicate", Type::kBlockPredicate},
    {"item", Type::kItem},
    {"item_predicate", Type::kItemPredicate},
    {"identifier", Type::kIdentifier},
    {"sound", Type::kSound},
    {"summonable", Type::kSummonable},
    {"message", Type::kMessage},
    {"anchor", Type::kAnchor},
};

bool numeric(Type t) { return t == Type::kInt || t == Type::kFloat || t == Type::kDouble; }

}  // namespace

i32 Invocation::which(std::string_view alternatives) const {
    for (i32 i = 0; !alternatives.empty(); ++i) {
        const usize bar = alternatives.find('|');
        if (has(alternatives.substr(0, bar))) return i;
        alternatives = bar == std::string_view::npos ? std::string_view() : alternatives.substr(bar + 1);
    }
    return -1;
}

Registry::Registry() { nodes_.emplace_back(); }

// The child of `parent` that is `like` (CommandNode.addChild: a node of that name is merged
// into), made if it is not there.
u32 Registry::child(u32 parent, const Node& like, std::string& err) {
    for (const u32 c : nodes_[parent].children) {
        const Node& n = nodes_[c];
        if (n.name != like.name) continue;
        if (n.kind != like.kind ||
            (n.kind == Node::kArgument && (n.type != like.type || n.bounded_min != like.bounded_min ||
                                           n.bounded_max != like.bounded_max || n.min != like.min || n.max != like.max))) {
            err = "'" + like.name + "' is already there as something else";
            return kNone;
        }
        return c;
    }
    const u32 id = static_cast<u32>(nodes_.size());
    nodes_.push_back(like);
    nodes_[parent].children.push_back(id);
    return id;
}

bool Registry::add(std::string_view syntax, Level level, Handler run, void* user, std::string& err) {
    std::vector<u32> current{0};
    const auto executable = [&]() {
        for (const u32 n : current) {
            if (n == 0) {
                err = "a command needs a name";
                return false;
            }
            if (nodes_[n].run && (nodes_[n].run != run || nodes_[n].user != user)) {
                err = "'" + nodes_[n].name + "' already runs another handler";
                return false;
            }
            nodes_[n].run = run;
            nodes_[n].user = user;
        }
        return true;
    };
    bool first = true;
    usize i = 0;
    while (true) {
        while (i < syntax.size() && syntax[i] == ' ') ++i;
        if (i == syntax.size()) break;
        usize end = syntax.find(' ', i);
        if (end == std::string_view::npos) end = syntax.size();
        std::string_view token = syntax.substr(i, end - i);
        i = end;
        while (token.starts_with('[')) {
            if (!executable()) return false;
            token.remove_prefix(1);
        }
        while (token.ends_with(']')) token.remove_suffix(1);
        if (token.empty()) continue;

        std::vector<Node> wanted;
        if (token.starts_with('<') && token.ends_with('>')) {
            token = token.substr(1, token.size() - 2);
            const usize colon = token.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                err = "an argument is written <name:type>";
                return false;
            }
            Node n;
            n.kind = Node::kArgument;
            n.name = token.substr(0, colon);
            std::string_view type = token.substr(colon + 1), bounds;
            if (const usize paren = type.find('('); paren != std::string_view::npos && type.ends_with(')')) {
                bounds = type.substr(paren + 1, type.size() - paren - 2);
                type = type.substr(0, paren);
            }
            const auto known =
                std::find_if(std::begin(kTypes), std::end(kTypes), [&](const TypeName& t) { return t.name == type; });
            if (known == std::end(kTypes)) {
                err = "unknown argument type '" + std::string(type) + "'";
                return false;
            }
            n.type = known->type;
            if (!bounds.empty()) {
                if (!numeric(n.type)) {
                    err = "only numbers take bounds";
                    return false;
                }
                const usize comma = bounds.find(',');
                const std::string lo(bounds.substr(0, comma));
                const std::string hi(comma == std::string_view::npos ? std::string_view() : bounds.substr(comma + 1));
                if (!lo.empty()) n.bounded_min = true, n.min = std::strtod(lo.c_str(), nullptr);
                if (!hi.empty()) n.bounded_max = true, n.max = std::strtod(hi.c_str(), nullptr);
            }
            wanted.push_back(std::move(n));
        } else {
            if (token.starts_with('(') && token.ends_with(')')) token = token.substr(1, token.size() - 2);
            while (!token.empty()) {
                const usize bar = token.find('|');
                Node n;
                n.kind = Node::kLiteral;
                n.name = token.substr(0, bar);
                if (n.name.empty()) {
                    err = "an empty word";
                    return false;
                }
                wanted.push_back(std::move(n));
                token = bar == std::string_view::npos ? std::string_view() : token.substr(bar + 1);
            }
        }
        if (first && (wanted.size() != 1 || wanted[0].kind != Node::kLiteral)) {
            err = "a command starts with its name";
            return false;
        }
        std::vector<u32> next;
        for (const u32 parent : current) {
            for (Node& n : wanted) {
                const bool is_new = first && std::none_of(nodes_[0].children.begin(), nodes_[0].children.end(),
                                                          [&](u32 c) { return nodes_[c].name == n.name; });
                if (first) n.level = level;
                const u32 c = child(parent, n, err);
                if (c == kNone) return false;
                if (first && !is_new && nodes_[c].level != level) {
                    err = "'" + n.name + "' is registered with another permission level";
                    return false;
                }
                next.push_back(c);
            }
        }
        current = std::move(next);
        first = false;
    }
    if (!executable()) return false;
    ++version_;
    return true;
}

void Registry::add(std::string_view syntax, Level level, Handler run, void* user) {
    std::string err;
    if (!add(syntax, level, run, user, err)) {
        LOG_ERROR("command '%.*s': %s", static_cast<int>(syntax.size()), syntax.data(), err.c_str());
        std::abort();
    }
}

bool Registry::alias(std::string_view name, std::string_view target, Level level) {
    u32 to = kNone;
    for (const u32 c : nodes_[0].children) {
        if (nodes_[c].name == name) return false;
        if (nodes_[c].name == target) to = c;
    }
    if (to == kNone) return false;
    Node n;
    n.kind = Node::kLiteral;
    n.name = name;
    n.level = level;
    n.redirect = to;
    nodes_[0].children.push_back(static_cast<u32>(nodes_.size()));
    nodes_.push_back(std::move(n));
    ++version_;
    return true;
}

bool Registry::remove(std::string_view name) {
    std::vector<u32>& top = nodes_[0].children;
    const auto it = std::find_if(top.begin(), top.end(), [&](u32 c) { return nodes_[c].name == name; });
    if (it == top.end()) return false;
    const u32 gone = *it;
    std::erase_if(top, [&](u32 c) { return c == gone || nodes_[c].redirect == gone; });
    ++version_;
    return true;
}

// ---------------------------------------------------------------------------- reading a line

// ParseResults: how far a reading got - its CommandContextBuilder, where its reader stands,
// and what the children it tried last threw.
struct Registry::Reading {
    std::vector<Parsed::Step> steps;
    Handler run = nullptr;
    void* user = nullptr;
    u32 cursor = 0;
    u32 last_node = 0;
    std::vector<Error> errors;
};

// ArgumentCommandNode.parse -> the argument type's parse.
bool Registry::parse_argument(const Node& n, std::string_view line, u32& cursor, u8 level, Value& out, Error& err) const {
    Reader r{line, cursor};
    out.type = n.type;
    out.start = cursor;
    bool ok = true;
    switch (n.type) {
        case Type::kBool: {  // StringReader.readBoolean
            const u32 start = r.at;
            std::string word;
            if (!read_string(r, word, err)) return false;
            if (word.empty()) {
                err = error_at(r, Text::translatable("parsing.bool.expected"));
                return false;
            }
            if (word != "true" && word != "false") {
                r.at = start;
                err = error_at(r, Text::translatable("parsing.bool.invalid").arg(word));
                return false;
            }
            out.flag = word == "true";
            break;
        }
        case Type::kInt: {  // IntegerArgumentType.parse
            const u32 start = r.at;
            if (!read_int(r, out.integer, err)) return false;
            const i32 lo = static_cast<i32>(n.min), hi = static_cast<i32>(n.max);
            if (n.bounded_min && out.integer < lo) {
                r.at = start;
                err = error_at(r, Text::translatable("argument.integer.low").arg(lo).arg(out.integer));
                return false;
            }
            if (n.bounded_max && out.integer > hi) {
                r.at = start;
                err = error_at(r, Text::translatable("argument.integer.big").arg(hi).arg(out.integer));
                return false;
            }
            out.number = out.integer;
            break;
        }
        case Type::kFloat:
        case Type::kDouble: {  // FloatArgumentType / DoubleArgumentType.parse
            const bool f = n.type == Type::kFloat;
            const u32 start = r.at;
            if (!read_double(r, out.number, err, f)) return false;
            const bool low = n.bounded_min && out.number < (f ? static_cast<double>(static_cast<float>(n.min)) : n.min);
            const bool big = n.bounded_max && out.number > (f ? static_cast<double>(static_cast<float>(n.max)) : n.max);
            if (low || big) {
                r.at = start;
                Text t = Text::translatable(f ? (low ? "argument.float.low" : "argument.float.big")
                                              : (low ? "argument.double.low" : "argument.double.big"));
                const double bound = low ? n.min : n.max;
                if (f)
                    t.arg(static_cast<float>(bound)).arg(static_cast<float>(out.number));
                else
                    t.arg(bound).arg(out.number);
                err = error_at(r, std::move(t));
                return false;
            }
            break;
        }
        case Type::kWord: out.text = read_unquoted(r); break;
        case Type::kString: ok = read_string(r, out.text, err); break;
        case Type::kGreedy:
            out.text = r.rest();
            r.at = static_cast<u32>(line.size());
            break;
        case Type::kMessage: {  // MessageArgument.Message.parseText
            const std::string_view rest = r.rest();
            if (const u32 length = proto::utf16_length(rest); length > kMessageMaxChars) {
                err = {Text::translatable("argument.message.too_long").arg(length).arg(kMessageMaxChars),
                       -1};  // (create: no place)
                return false;
            }
            out.text = rest;
            const u32 offset = r.at;
            r.at = static_cast<u32>(line.size());
            // EntitySelectorParser.allowSelectors(source): only who may use selectors has them
            // looked for. An '@' that is followed by no selector type is text; any other mistake
            // in a selector is the message's.
            for (u32 at = offset; level >= kGamemasters && at < line.size();) {
                if (line[at] != '@') {
                    ++at;
                    continue;
                }
                const u32 start = at;
                MessagePart part;
                Error why;
                if (parse_selector(line, at, Type::kEntities, true, part.selector, why)) {
                    part.start = start - offset;
                    part.end = at - offset;
                    out.parts.push_back(std::move(part));
                } else if (why.kind == Error::kNoSelectorType) {
                    at = start + 1;
                } else {
                    err = std::move(why);
                    return false;
                }
            }
            break;
        }
        case Type::kIdentifier:
        case Type::kSound: ok = read_identifier(r, out.text, err); break;
        case Type::kSummonable: {  // ResourceArgument.parse over minecraft:entity_type
            if (!read_identifier(r, out.text, err)) return false;
            const u32 type = world::find_entity_type(out.text);
            if (type == world::kNoId) {  // (the cursor stays behind the name: vanilla does not put it back)
                err = error_at(r, Text::translatable("argument.resource.not_found").arg(out.text).arg("minecraft:entity_type"));
                return false;
            }
            out.integer = static_cast<i32>(type);
            break;
        }
        case Type::kAnchor: {  // EntityAnchorArgument.parse
            const u32 start = r.at;
            const std::string_view name = read_unquoted(r);
            if (name != "feet" && name != "eyes") {
                r.at = start;
                err = error_at(r, Text::translatable("argument.anchor.invalid").arg(name));
                return false;
            }
            out.text = name;
            out.flag = name == "eyes";
            break;
        }
        case Type::kBlockPos:
        case Type::kVec3:
        case Type::kRotation: ok = parse_coordinates(line, r.at, n.type, out.coords, err); break;
        case Type::kEntity:
        case Type::kEntities:
        case Type::kPlayer:
        case Type::kPlayers:
        case Type::kProfile:
            // Permissions.COMMANDS_ENTITY_SELECTORS: gamemasters and up may write '@'.
            ok = parse_selector(line, r.at, n.type, level >= kGamemasters, out.selector, err);
            break;
        case Type::kBlockState:
        case Type::kBlockPredicate: ok = parse_block(line, r.at, n.type == Type::kBlockPredicate, out.block, err); break;
        case Type::kItem:
        case Type::kItemPredicate: ok = parse_item(line, r.at, n.type == Type::kItemPredicate, out.item, err); break;
    }
    if (!ok) return false;
    cursor = r.at;
    out.end = cursor;
    return true;
}

// CommandDispatcher.parseNodes
void Registry::parse_nodes(u32 node, std::string_view line, u32 cursor, u8 level, const Reading& so_far, Reading& out) const {
    const Node& parent = nodes_[node];
    // CommandNode.getRelevantNodes: a literal child spelled exactly like the next word is the
    // only candidate; otherwise the arguments are, in the order they were added.
    u32 word_end = cursor;
    while (word_end < line.size() && line[word_end] != ' ') ++word_end;
    const std::string_view word = line.substr(cursor, word_end - cursor);
    u32 literal = kNone;
    for (const u32 c : parent.children)
        if (nodes_[c].kind == Node::kLiteral && nodes_[c].name == word) literal = c;

    std::vector<Error> errors;
    std::vector<Reading> potentials;
    for (const u32 c : parent.children) {
        const Node& n = nodes_[c];
        if (literal != kNone ? c != literal : n.kind != Node::kArgument) continue;
        if (n.level > level) continue;  // canUse
        Reading reading = so_far;
        u32 at = cursor;
        Error err;
        bool ok = true;
        Parsed::Step step{c, false, {}};
        if (n.kind == Node::kLiteral) {
            at = word_end;  // LiteralCommandNode.parse: the word, then a space or the end
        } else {
            step.has_value = true;
            ok = parse_argument(n, line, at, level, step.value, err);
        }
        if (ok && at < line.size() && line[at] != ' ') {
            err = {Text::translatable("command.expected.separator"), static_cast<i32>(at)};
            ok = false;
        }
        if (!ok) {
            errors.push_back(std::move(err));
            continue;
        }
        reading.steps.push_back(std::move(step));
        reading.run = n.run;  // withCommand: also when the node has none
        reading.user = n.user;
        reading.last_node = c;
        reading.cursor = at;
        reading.errors.clear();
        if (at + (n.redirect == kNone ? 2u : 1u) <= line.size()) {
            ++at;  // the separator
            if (n.redirect != kNone) {
                // A new context under the node redirected to; its command is the one that counts.
                Reading child;
                child.cursor = at;
                Reading deeper;
                parse_nodes(n.redirect, line, at, level, child, deeper);
                reading.steps.insert(reading.steps.end(), std::make_move_iterator(deeper.steps.begin()),
                                     std::make_move_iterator(deeper.steps.end()));
                reading.run = deeper.run;
                reading.user = deeper.user;  // (last_node stays the redirecting node: the top context ends there)
                reading.cursor = deeper.cursor;
                reading.errors = std::move(deeper.errors);
                out = std::move(reading);
                return;
            }
            Reading deeper;
            parse_nodes(c, line, at, level, reading, deeper);
            potentials.push_back(std::move(deeper));
        } else {
            potentials.push_back(std::move(reading));
        }
    }
    if (!potentials.empty()) {
        // The reading that used the whole line comes first, then one without errors; otherwise
        // the first that was tried.
        std::stable_sort(potentials.begin(), potentials.end(), [&](const Reading& a, const Reading& b) {
            const bool a_rest = a.cursor < line.size(), b_rest = b.cursor < line.size();
            if (a_rest != b_rest) return !a_rest;
            return a.errors.empty() && !b.errors.empty();
        });
        out = std::move(potentials[0]);
        return;
    }
    out = so_far;
    out.cursor = cursor;
    out.errors = std::move(errors);
}

void Registry::parse(std::string_view line, u8 level, Parsed& out) const {
    Reading start, got;
    parse_nodes(0, line, 0, level, start, got);
    out.steps = std::move(got.steps);
    out.any = !out.steps.empty();
    out.last_node = got.last_node;
    out.run = nullptr;
    out.user = got.user;
    // Commands.getParseException: something is left unread.
    if (got.cursor < line.size()) {
        if (got.errors.size() == 1)
            out.error = std::move(got.errors[0]);
        else
            out.error = {Text::translatable(out.any ? "command.unknown.argument" : "command.unknown.command"),
                         static_cast<i32>(got.cursor)};
        return;
    }
    // ContextChain.tryFlatten: the line ends on a node that runs nothing.
    if (!got.run) {
        out.error = {Text::translatable("command.unknown.command"), static_cast<i32>(got.cursor)};
        return;
    }
    out.run = got.run;
}

// Commands.finishParsing: "...stone extra<--[HERE]" - grey, clickable back into the chat box.
text::Text error_context(std::string_view line, i32 at) {
    const usize cursor = std::min(line.size(), static_cast<usize>(std::max(at, 0)));
    // Ten chars back - Java's, UTF-16 units: a code point outside the BMP counts twice.
    usize from = cursor;
    u32 units = 0;
    while (from > 0 && units < 10) {
        usize prev = from - 1;
        while (prev > 0 && (static_cast<u8>(line[prev]) & 0xC0) == 0x80) --prev;
        const u32 width = from - prev == 4 ? 2 : 1;
        if (units + width > 10) break;
        units += width;
        from = prev;
    }
    std::string command("/");
    command += line;
    Text context = Text().color(text::Color::kGray).suggest_command(command);
    if (from > 0) context.append("...");
    context.append(line.substr(from, cursor - from));
    if (cursor < line.size()) context.append(Text::literal(line.substr(cursor)).color(text::Color::kRed).underlined());
    context.append(Text::translatable("command.context.here").color(text::Color::kRed).italic());
    return context;
}

// ---------------------------------------------------------------------------- the packet

// ClientboundCommandsPacket: the nodes a source of this level can use, numbered breadth first
// from the root (enumerateNodes), each with its children's numbers.
void Registry::encode_tree(u8 level, base::ByteBuf& out) const {
    using namespace proto;
    std::vector<u32> order;
    std::vector<u32> number(nodes_.size(), kNone);
    std::deque<u32> queue{0};
    const auto usable = [&](u32 c) { return nodes_[c].level <= level; };
    while (!queue.empty()) {
        const u32 n = queue.front();
        queue.pop_front();
        if (number[n] != kNone) continue;
        number[n] = static_cast<u32>(order.size());
        order.push_back(n);
        for (const u32 c : nodes_[n].children)
            if (usable(c)) queue.push_back(c);
        if (nodes_[n].redirect != kNone) queue.push_back(nodes_[n].redirect);
    }
    const auto put = [&](usize bound, auto&& write) { out.commit(write(out.reserve(bound))); };
    put(10, [&](u8* p) { return put_varint(put_varint(p, ids::play::cb::kCommands), static_cast<u32>(order.size())); });
    for (const u32 id : order) {
        const Node& n = nodes_[id];
        u8 flags = static_cast<u8>(n.kind);  // TYPE_ROOT, TYPE_LITERAL, TYPE_ARGUMENT
        if (n.run) flags |= 4;               // FLAG_EXECUTABLE
        if (n.redirect != kNone) flags |= 8;
        const std::string_view suggestions = n.kind != Node::kArgument     ? std::string_view()
                                             : n.type == Type::kSound      ? "minecraft:available_sounds"
                                             : n.type == Type::kSummonable ? "minecraft:summonable_entities"
                                                                           : std::string_view();
        if (!suggestions.empty()) flags |= 16;  // FLAG_CUSTOM_SUGGESTIONS
        if (n.level > kAll) flags |= 32;        // FLAG_RESTRICTED: not for a source without permissions
        u32 children = 0;
        for (const u32 c : n.children) children += usable(c);
        put(1 + 5 + children * 5 + 5 + 5 + n.name.size() + 64, [&](u8* p) {
            p = put_varint(put_u8(p, flags), children);
            for (const u32 c : n.children)
                if (usable(c)) p = put_varint(p, number[c]);
            if (n.redirect != kNone) p = put_varint(p, number[n.redirect]);
            if (n.kind == Node::kRoot) return p;
            p = put_str(p, n.name);
            if (n.kind == Node::kLiteral) return p;
            const u8 bounds =
                static_cast<u8>((n.bounded_min ? 1 : 0) | (n.bounded_max ? 2 : 0));  // ArgumentUtils.createNumberFlags
            switch (n.type) {
                case Type::kBool: p = put_varint(p, argument::kBool); break;
                case Type::kInt:
                    p = put_u8(put_varint(p, argument::kInteger), bounds);
                    if (n.bounded_min) p = put_be32(p, static_cast<u32>(static_cast<i32>(n.min)));
                    if (n.bounded_max) p = put_be32(p, static_cast<u32>(static_cast<i32>(n.max)));
                    break;
                case Type::kFloat:
                    p = put_u8(put_varint(p, argument::kFloat), bounds);
                    if (n.bounded_min) p = put_f32(p, static_cast<float>(n.min));
                    if (n.bounded_max) p = put_f32(p, static_cast<float>(n.max));
                    break;
                case Type::kDouble:
                    p = put_u8(put_varint(p, argument::kDouble), bounds);
                    if (n.bounded_min) p = put_f64(p, n.min);
                    if (n.bounded_max) p = put_f64(p, n.max);
                    break;
                // StringArgumentType.StringType: SINGLE_WORD, QUOTABLE_PHRASE, GREEDY_PHRASE
                case Type::kWord: p = put_varint(put_varint(p, argument::kString), 0); break;
                case Type::kString: p = put_varint(put_varint(p, argument::kString), 1); break;
                case Type::kGreedy: p = put_varint(put_varint(p, argument::kString), 2); break;
                // EntityArgument.Info: bit 0 a single one, bit 1 players only
                case Type::kEntity: p = put_u8(put_varint(p, argument::kEntity), 1); break;
                case Type::kEntities: p = put_u8(put_varint(p, argument::kEntity), 0); break;
                case Type::kPlayer: p = put_u8(put_varint(p, argument::kEntity), 3); break;
                case Type::kPlayers: p = put_u8(put_varint(p, argument::kEntity), 2); break;
                case Type::kProfile: p = put_varint(p, argument::kGameProfile); break;
                case Type::kBlockPos: p = put_varint(p, argument::kBlockPos); break;
                case Type::kVec3: p = put_varint(p, argument::kVec3); break;
                case Type::kRotation: p = put_varint(p, argument::kRotation); break;
                case Type::kBlockState: p = put_varint(p, argument::kBlockState); break;
                case Type::kBlockPredicate: p = put_varint(p, argument::kBlockPredicate); break;
                case Type::kItem: p = put_varint(p, argument::kItemStack); break;
                case Type::kItemPredicate: p = put_varint(p, argument::kItemPredicate); break;
                case Type::kIdentifier:
                case Type::kSound: p = put_varint(p, argument::kResourceLocation); break;
                case Type::kSummonable: p = put_str(put_varint(p, argument::kResource), "minecraft:entity_type"); break;
                case Type::kMessage: p = put_varint(p, argument::kMessage); break;
                case Type::kAnchor: p = put_varint(p, argument::kEntityAnchor); break;
            }
            return suggestions.empty() ? p : put_str(p, suggestions);
        });
    }
    put(5, [&](u8* p) { return put_varint(p, 0); });  // rootIndex
}

// ---------------------------------------------------------------------------- usage

std::string Registry::usage_text(u32 node) const {
    const Node& n = nodes_[node];
    return n.kind == Node::kArgument ? "<" + n.name + ">" : n.name;
}

// CommandDispatcher.getSmartUsage(node, source, optional, deep)
std::string Registry::smart_usage(u32 node, u8 level, bool optional, bool deep) const {
    const Node& n = nodes_[node];
    const std::string self = optional ? "[" + usage_text(node) + "]" : usage_text(node);
    const bool child_optional = n.run != nullptr;
    if (deep) return self;
    if (n.redirect != kNone) return self + " " + (n.redirect == 0 ? std::string("...") : "-> " + usage_text(n.redirect));
    std::vector<u32> children;
    for (const u32 c : n.children)
        if (nodes_[c].level <= level) children.push_back(c);
    if (children.size() == 1) return self + " " + smart_usage(children[0], level, child_optional, child_optional);
    if (children.size() > 1) {
        std::vector<std::string> usages;
        for (const u32 c : children) {
            std::string u = smart_usage(c, level, child_optional, true);
            if (std::find(usages.begin(), usages.end(), u) == usages.end()) usages.push_back(std::move(u));
        }
        if (usages.size() == 1) return self + " " + (child_optional ? "[" + usages[0] + "]" : usages[0]);
        std::string all = child_optional ? "[" : "(";
        for (usize i = 0; i < children.size(); ++i) {
            if (i) all += "|";
            all += usage_text(children[i]);
        }
        all += child_optional ? "]" : ")";
        return self + " " + all;
    }
    return self;
}

bool Registry::usage(std::string_view line, u8 level, std::vector<std::string>& out) const {
    u32 node = 0;
    if (!line.empty()) {
        Parsed parsed;
        parse(line, level, parsed);
        if (!parsed.any) return false;
        node = parsed.last_node;
    }
    const bool optional = nodes_[node].run != nullptr;
    for (const u32 c : nodes_[node].children)
        if (nodes_[c].level <= level) out.push_back(smart_usage(c, level, optional, false));
    return true;
}

}  // namespace server::cmd
