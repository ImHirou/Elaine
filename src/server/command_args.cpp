// SPDX-License-Identifier: GPL-3.0-or-later
// The argument types that are vanilla's own: coordinates, entity selectors, block states and
// items. Each is the port of its parser in net.minecraft.commands.arguments (26.3) - the same
// steps over the reader, the same error at the same cursor - named at each function.
//
// What is NOT read, and answered with an error of this server's own instead of vanilla's
// "expected ...":
//   * NBT: a block's `{...}`, an item's `[component=value]`, a selector's `nbt=`;
//   * the selector options that ask what does not exist here: scores, advancements, predicate.
// A selector's `tag` and `team` are read and compared with what every entity has: none.
#include <algorithm>
#include <charconv>
#include <cstdlib>

#include "proto/gen/current.hpp"
#include "proto/identifier.hpp"
#include "proto/static_data.hpp"
#include "proto/wire.hpp"
#include "server/command.hpp"

#include PROTO_GEN_ENTITIES_HPP

namespace server::cmd {

using text::Text;

namespace {

struct Reader {
    std::string_view s;
    u32 at;
    bool can_read(u32 n = 1) const { return at + n <= s.size(); }
    char peek(u32 ahead = 0) const { return s[at + ahead]; }
    void skip_whitespace() {
        while (can_read() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
            ++at;  // Character.isWhitespace
    }
};

Error here(const Reader& r, Text message) { return {std::move(message), static_cast<i32>(r.at)}; }
Error at(u32 cursor, Text message) { return {std::move(message), static_cast<i32>(cursor)}; }
Text tr(std::string_view key) { return Text::translatable(key); }
// Not vanilla's: something its parser reads and this server does not.
Text unsupported(std::string_view what) {
    std::string s(what);
    s += " are not supported by this server yet";
    return Text::literal(s);
}

bool allowed_number(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-'; }
bool allowed_unquoted(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.' ||
           c == '+';
}
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
double to_double(std::string_view t) {
    const std::string z(t);
    return std::strtod(z.c_str(), nullptr);
}

// StringReader.readDouble / readInt (as in command.cpp; the errors are Brigadier's).
bool read_double(Reader& r, double& out, Error& err) {
    const u32 start = r.at;
    while (r.can_read() && allowed_number(r.peek())) ++r.at;
    const std::string_view t = r.s.substr(start, r.at - start);
    if (t.empty()) {
        err = here(r, tr("parsing.double.expected"));
        return false;
    }
    if (!plain_decimal(t)) {
        r.at = start;
        err = here(r, tr("parsing.double.invalid").arg(t));
        return false;
    }
    out = to_double(t);
    return true;
}
bool read_int(Reader& r, i32& out, Error& err) {
    const u32 start = r.at;
    while (r.can_read() && allowed_number(r.peek())) ++r.at;
    const std::string_view t = r.s.substr(start, r.at - start);
    if (t.empty()) {
        err = here(r, tr("parsing.int.expected"));
        return false;
    }
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
    if (ec != std::errc() || end != t.data() + t.size()) {
        r.at = start;
        err = here(r, tr("parsing.int.invalid").arg(t));
        return false;
    }
    return true;
}
std::string_view read_unquoted(Reader& r) {
    const u32 start = r.at;
    while (r.can_read() && allowed_unquoted(r.peek())) ++r.at;
    return r.s.substr(start, r.at - start);
}
bool read_string(Reader& r, std::string& out, Error& err) {  // StringReader.readString
    out.clear();
    if (!r.can_read()) return true;
    const char quote = r.peek();
    if (quote != '"' && quote != '\'') {
        out = read_unquoted(r);
        return true;
    }
    ++r.at;
    bool escaped = false;
    while (r.can_read()) {
        const char c = r.s[r.at++];
        if (escaped) {
            if (c != quote && c != '\\') {
                --r.at;
                err = here(r, tr("parsing.quote.escape").arg(std::string_view(&c, 1)));
                return false;
            }
            out += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == quote) {
            return true;
        } else {
            out += c;
        }
    }
    err = here(r, tr("parsing.quote.expected.end"));
    return false;
}
bool read_identifier(Reader& r, std::string& out, Error& err) {  // Identifier.read
    const u32 start = r.at;
    const auto allowed = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c == '/' || c == '.' || c == '-';
    };
    while (r.can_read() && allowed(r.peek())) ++r.at;
    proto::Identifier id;
    if (!proto::parse_identifier(r.s.substr(start, r.at - start), id)) {
        r.at = start;
        err = here(r, tr("argument.id.invalid"));
        return false;
    }
    out.assign(id.ns);
    out += ':';
    out += id.path;
    return true;
}

// ---------------------------------------------------------------------------- coordinates

// WorldCoordinate.parseDouble
bool world_double(Reader& r, bool center, Coordinate& out, Error& err) {
    if (r.can_read() && r.peek() == '^') {
        err = here(r, tr("argument.pos.mixed"));
        return false;
    }
    if (!r.can_read()) {
        err = here(r, tr("argument.pos.missing.double"));
        return false;
    }
    out.relative = r.peek() == '~';
    if (out.relative) ++r.at;
    const u32 start = r.at;
    out.value = 0;
    if (r.can_read() && r.peek() != ' ' && !read_double(r, out.value, err)) return false;
    const std::string_view number = r.s.substr(start, r.at - start);
    // A whole number names a block: its middle is meant.
    if (!out.relative && center && number.find('.') == std::string_view::npos) out.value += 0.5;
    return true;
}
// WorldCoordinate.parseInt
bool world_int(Reader& r, Coordinate& out, Error& err) {
    if (r.can_read() && r.peek() == '^') {
        err = here(r, tr("argument.pos.mixed"));
        return false;
    }
    if (!r.can_read()) {
        err = here(r, tr("argument.pos.missing.int"));
        return false;
    }
    out.relative = r.peek() == '~';
    if (out.relative) ++r.at;
    out.value = 0;
    if (!r.can_read() || r.peek() == ' ') return true;
    if (out.relative) return read_double(r, out.value, err);
    i32 whole = 0;
    if (!read_int(r, whole, err)) return false;
    out.value = whole;
    return true;
}
// LocalCoordinates.readDouble
bool local_double(Reader& r, u32 start, Coordinate& out, Error& err) {
    if (!r.can_read()) {
        err = here(r, tr("argument.pos.missing.double"));
        return false;
    }
    if (r.peek() != '^') {
        r.at = start;
        err = here(r, tr("argument.pos.mixed"));
        return false;
    }
    ++r.at;
    out.relative = true;
    out.value = 0;
    return !r.can_read() || r.peek() == ' ' || read_double(r, out.value, err);
}

}  // namespace

// BlockPosArgument.parse, Vec3Argument.parse, RotationArgument.parse
bool parse_coordinates(std::string_view line, u32& cursor, Type type, Coordinates& out, Error& err) {
    Reader r{line, cursor};
    const u32 start = r.at;
    const u32 parts = type == Type::kRotation ? 2 : 3;
    const std::string_view incomplete = type == Type::kRotation ? "argument.rotation.incomplete" : "argument.pos3d.incomplete";
    out.local = type != Type::kRotation && r.can_read() && r.peek() == '^';
    if (type == Type::kRotation && !r.can_read()) {
        err = here(r, tr(incomplete));
        return false;
    }
    for (u32 i = 0; i < parts; ++i) {
        if (i > 0) {  // "x y z": each but the last is followed by a space
            if (!r.can_read() || r.peek() != ' ') {
                r.at = start;
                err = here(r, tr(incomplete));
                return false;
            }
            ++r.at;
        }
        const bool ok = out.local                 ? local_double(r, start, out.c[i], err)
                        : type == Type::kBlockPos ? world_int(r, out.c[i], err)
                        : type == Type::kRotation ? world_double(r, false, out.c[i], err)
                                                  : world_double(r, i != 1, out.c[i], err);  // x and z are centred, y is not
        if (!ok) return false;
    }
    cursor = r.at;
    return true;
}

// ---------------------------------------------------------------------------- tags

namespace {

struct TagTable {
    struct Tag {
        std::string registry, name;
        std::vector<u16> ids;
    };
    std::vector<Tag> tags;
    TagTable() {
        const std::span<const u8> body = proto::tags_body();
        proto::Reader r(body.data(), body.size());
        r.varint();  // the packet id
        const u32 registries = r.varint();
        for (u32 i = 0; i < registries && r.ok; ++i) {
            const std::string_view registry = r.str(32767);
            const u32 count = r.varint();
            for (u32 t = 0; t < count && r.ok; ++t) {
                Tag tag{std::string(registry), std::string(r.str(32767)), {}};
                const u32 n = r.varint();
                for (u32 k = 0; k < n && r.ok; ++k) tag.ids.push_back(static_cast<u16>(r.varint()));
                tags.push_back(std::move(tag));
            }
        }
        if (!r.done()) std::abort();  // generated data is corrupt
    }
};

}  // namespace

bool tag_members(std::string_view registry, std::string_view tag, std::vector<u16>& out) {
    static const TagTable table;
    for (const TagTable::Tag& t : table.tags) {
        if (t.registry != registry || t.name != tag) continue;
        out = t.ids;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------- entity selectors

namespace {

// MinMaxBounds.Bounds.fromReader: "1", "1..2", "..2", "1.."
bool read_range(Reader& r, bool integers, bool may_swap, Range& out, Error& err) {
    if (!r.can_read()) {
        err = here(r, tr("argument.range.empty"));
        return false;
    }
    const u32 start = r.at;
    const std::string_view invalid = integers ? "parsing.int.invalid" : "parsing.double.invalid";
    const auto number = [&](double& value, bool& present) {  // readNumber
        const u32 from = r.at;
        // isAllowedInputChar: a digit, a minus, or a point that does not begin "..".
        while (r.can_read()) {
            const char c = r.peek();
            const bool ok = (c >= '0' && c <= '9') || c == '-' || (c == '.' && !(r.can_read(2) && r.peek(1) == '.'));
            if (!ok) break;
            ++r.at;
        }
        const std::string_view t = r.s.substr(from, r.at - from);
        present = !t.empty();
        if (!present) return true;
        i32 whole = 0;
        const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), whole);
        if (integers ? (ec != std::errc() || end != t.data() + t.size()) : !plain_decimal(t)) {
            err = at(start, tr(invalid).arg(t));  // rethrown at the range's start
            return false;
        }
        value = integers ? whole : to_double(t);
        return true;
    };
    if (!number(out.min, out.has_min)) return false;
    if (r.can_read(2) && r.peek() == '.' && r.peek(1) == '.') {
        r.at += 2;
        if (!number(out.max, out.has_max)) return false;
    } else {
        out.max = out.min;
        out.has_max = out.has_min;
    }
    if (!out.has_min && !out.has_max) {
        err = at(start, tr("argument.range.empty"));
        return false;
    }
    if (!may_swap && out.has_min && out.has_max && out.min > out.max) {
        err = at(start, tr("argument.range.swapped"));
        return false;
    }
    return true;
}

// UUID.fromString: five groups of hex digits; each is read as a number and cut to its width.
bool parse_uuid(std::string_view s, Uuid& out) {
    u64 part[5];
    u32 n = 0;
    while (true) {
        const usize dash = s.find('-');
        const std::string_view group = s.substr(0, dash);
        if (n == 5 || group.empty() || group.size() > 16) return false;
        const auto [end, ec] = std::from_chars(group.data(), group.data() + group.size(), part[n], 16);
        if (ec != std::errc() || end != group.data() + group.size()) return false;
        ++n;
        if (dash == std::string_view::npos) break;
        s = s.substr(dash + 1);
    }
    if (n != 5) return false;
    out.hi = (part[0] & 0xFFFFFFFFull) << 32 | (part[1] & 0xFFFFull) << 16 | (part[2] & 0xFFFFull);
    out.lo = (part[3] & 0xFFFFull) << 48 | (part[4] & 0xFFFFFFFFFFFFull);
    return true;
}

// InvertableSetOptionState: an option that may be given once, or any number of times negated.
struct Invertable {
    enum : u8 { kNone, kSingle, kMultiple } state = kNone;
    std::vector<std::string> tags;
    bool can_parse_any() const { return state != kSingle; }
    bool can_parse(bool inverted) const { return inverted ? state != kSingle : state == kNone; }
    void mark(bool inverted) { state = inverted ? kMultiple : kSingle; }
};

struct SelectorParser {
    Reader& r;
    Selector& sel;
    Invertable name, gamemode, team, type;
    bool limited = false, sorted = false;

    // EntitySelectorParser.shouldInvertValue / isTag
    bool eat(char c) {
        r.skip_whitespace();
        if (!r.can_read() || r.peek() != c) return false;
        ++r.at;
        r.skip_whitespace();
        return true;
    }

    bool option(std::string_view key, u32 key_start, Error& err);
    bool options(Error& err);
    bool selector(Error& err);
    bool name_or_uuid(Error& err);
};

constexpr std::string_view kGameModes[] = {"survival", "creative", "adventure", "spectator"};  // GameType, by id

// EntitySelectorOptions.get, then the option's handler.
bool SelectorParser::option(std::string_view key, u32 key_start, Error& err) {
    const auto inapplicable = [&](u32 start, std::string_view what = {}) {
        err = at(start, tr("argument.entity.options.inapplicable").arg(what.empty() ? key : what));
        return false;
    };
    static constexpr std::string_view kAxes[] = {"x", "y", "z", "dx", "dy", "dz"};
    const u32 axis = static_cast<u32>(std::find(std::begin(kAxes), std::end(kAxes), key) - std::begin(kAxes));
    bool* const axis_set = axis < 3 ? &sel.has_pos[axis] : axis < 6 ? &sel.has_delta[axis - 3] : nullptr;
    const bool known = key == "name" || key == "distance" || key == "level" || key == "x_rotation" || key == "y_rotation" ||
                       key == "limit" || key == "sort" || key == "gamemode" || key == "team" || key == "type" || key == "tag" ||
                       key == "nbt" || key == "scores" || key == "advancements" || key == "predicate" || axis_set;
    if (!known) {
        err = at(key_start, tr("argument.entity.options.unknown").arg(key));
        return false;
    }
    // Whether the option may come (again) is decided before its '=' is asked for.
    const bool usable = axis_set              ? !*axis_set
                        : key == "name"       ? name.can_parse_any()
                        : key == "distance"   ? !sel.has_distance
                        : key == "level"      ? !sel.has_level
                        : key == "x_rotation" ? !sel.has_rot_x
                        : key == "y_rotation" ? !sel.has_rot_y
                        : key == "limit"      ? !sel.current_entity && !limited
                        : key == "sort"       ? !sel.current_entity && !sorted
                        : key == "gamemode"   ? gamemode.can_parse_any()
                        : key == "team"       ? team.can_parse_any()
                        : key == "type"       ? type.can_parse_any()
                                              : true;
    if (!usable) return inapplicable(key_start);
    // parseOptions: the key, then '='.
    r.skip_whitespace();
    if (!r.can_read() || r.peek() != '=') {
        err = at(key_start, tr("argument.entity.options.valueless").arg(key));
        return false;
    }
    ++r.at;
    r.skip_whitespace();

    const u32 start = r.at;
    if (axis_set) {
        sel.world_limited = true;
        *axis_set = true;
        return read_double(r, axis < 3 ? sel.pos[axis] : sel.delta[axis - 3], err);
    }
    if (key == "name") {
        const bool inverted = eat('!');
        std::string value;
        if (!read_string(r, value, err)) return false;
        if (!name.can_parse(inverted)) return inapplicable(start);
        name.mark(inverted);
        sel.tests.push_back({Selector::Test::kName, inverted, 0, std::move(value), {}});
    } else if (key == "distance" || key == "level") {
        const bool level = key == "level";
        Range& range = level ? sel.level : sel.distance;
        if (!read_range(r, level, false, range, err)) return false;
        if ((range.has_min && range.min < 0) || (range.has_max && range.max < 0)) {
            err = at(start, tr(level ? "argument.entity.options.level.negative" : "argument.entity.options.distance.negative"));
            return false;
        }
        (level ? sel.has_level : sel.has_distance) = true;
        if (level)
            sel.includes_entities = false;
        else
            sel.world_limited = true;
    } else if (key == "x_rotation" || key == "y_rotation") {
        const bool x = key == "x_rotation";
        if (!read_range(r, false, true, x ? sel.rot_x : sel.rot_y, err)) return false;
        (x ? sel.has_rot_x : sel.has_rot_y) = true;
    } else if (key == "limit") {
        i32 count = 0;
        if (!read_int(r, count, err)) return false;
        if (count < 1) {
            err = at(start, tr("argument.entity.options.limit.toosmall"));
            return false;
        }
        sel.max_results = count;
        limited = true;
    } else if (key == "sort") {
        const std::string_view how = read_unquoted(r);
        if (how == "nearest")
            sel.order = Selector::kNearest;
        else if (how == "furthest")
            sel.order = Selector::kFurthest;
        else if (how == "random")
            sel.order = Selector::kRandom;
        else if (how == "arbitrary")
            sel.order = Selector::kArbitrary;
        else {
            err = at(start, tr("argument.entity.options.sort.irreversible").arg(how));
            return false;
        }
        sorted = true;
    } else if (key == "gamemode") {
        const bool inverted = eat('!');
        if (!gamemode.can_parse(inverted)) return inapplicable(start);
        const std::string_view mode = read_unquoted(r);
        const auto found = std::find(std::begin(kGameModes), std::end(kGameModes), mode);
        if (found == std::end(kGameModes)) {
            err = at(start, tr("argument.entity.options.mode.invalid").arg(mode));
            return false;
        }
        sel.includes_entities = false;
        sel.tests.push_back({Selector::Test::kGamemode, inverted, static_cast<u32>(found - std::begin(kGameModes)), {}, {}});
        gamemode.mark(inverted);
    } else if (key == "team") {
        const bool inverted = eat('!');
        const std::string_view value = read_unquoted(r);
        if (!team.can_parse(inverted)) return inapplicable(start);
        sel.tests.push_back({Selector::Test::kTeam, inverted, 0, std::string(value), {}});
        team.mark(inverted);
    } else if (key == "type") {
        const bool inverted = eat('!');
        std::string id;
        if (eat('#')) {
            if (type.state == Invertable::kSingle) return inapplicable(start);
            if (!read_identifier(r, id, err)) return false;
            if (std::find(type.tags.begin(), type.tags.end(), id) != type.tags.end()) return inapplicable(start);
            Selector::Test test{Selector::Test::kTypeTag, inverted, 0, {}, {}};
            tag_members("minecraft:entity_type", id, test.ids);  // a tag nobody defined has no members
            sel.tests.push_back(std::move(test));
            type.state = Invertable::kMultiple;
            type.tags.push_back(std::move(id));
        } else {
            if (!type.can_parse(inverted)) return inapplicable(start);
            if (!read_identifier(r, id, err)) return false;
            const u32 found = world::find_entity_type(id);
            if (found == world::kNoId) {
                err = at(start, tr("argument.entity.options.type.invalid").arg(id));
                return false;
            }
            if (found == proto::gen::entity_type::kPlayer && !inverted) sel.includes_entities = false;
            sel.tests.push_back({Selector::Test::kType, inverted, found, {}, {}});
            if (!inverted) sel.type = found;
            type.mark(inverted);
        }
    } else if (key == "tag") {
        const bool inverted = eat('!');
        sel.tests.push_back({Selector::Test::kTag, inverted, 0, std::string(read_unquoted(r)), {}});
    } else {  // nbt, scores, advancements, predicate
        std::string what = "Selectors with '";
        what += key;
        what += "'";
        err = at(key_start, unsupported(what));
        return false;
    }
    return true;
}

// EntitySelectorParser.parseOptions: after the '['
bool SelectorParser::options(Error& err) {
    r.skip_whitespace();
    while (r.can_read() && r.peek() != ']') {
        r.skip_whitespace();
        const u32 start = r.at;
        std::string key;
        if (!read_string(r, key, err)) return false;
        if (!option(key, start, err)) return false;
        r.skip_whitespace();
        if (!r.can_read()) continue;
        if (r.peek() == ',') {
            ++r.at;
        } else if (r.peek() != ']') {
            err = here(r, tr("argument.entity.options.unterminated"));
            return false;
        }
    }
    if (!r.can_read()) {
        err = here(r, tr("argument.entity.options.unterminated"));
        return false;
    }
    ++r.at;
    return true;
}

// EntitySelectorParser.parseSelector: after the '@'
bool SelectorParser::selector(Error& err) {
    sel.uses_selector = true;
    if (!r.can_read()) {
        err = here(r, tr("argument.entity.selector.missing"));
        return false;
    }
    const u32 start = r.at;
    const char kind = r.s[r.at++];
    const auto players = [&](i32 max, Selector::Order order) {
        sel.max_results = max;
        sel.includes_entities = false;
        sel.order = order;
        sel.type = proto::gen::entity_type::kPlayer;
    };
    const auto entities = [&](i32 max, Selector::Order order) {
        sel.max_results = max;
        sel.includes_entities = true;
        sel.order = order;
        sel.tests.push_back({Selector::Test::kAlive, false, 0, {}, {}});
    };
    switch (kind) {
        case 'p': players(1, Selector::kNearest); break;
        case 'a': players(0x7FFFFFFF, Selector::kArbitrary); break;
        case 'r': players(1, Selector::kRandom); break;
        case 's':
            sel.max_results = 1;
            sel.includes_entities = true;
            sel.current_entity = true;
            break;
        case 'e': entities(0x7FFFFFFF, Selector::kArbitrary); break;
        case 'n': entities(1, Selector::kNearest); break;
        default: {
            r.at = start;
            std::string shown("@");
            shown += kind;
            err = here(r, tr("argument.entity.selector.unknown").arg(shown));
            return false;
        }
    }
    if (r.can_read() && r.peek() == '[') {
        ++r.at;
        return options(err);
    }
    return true;
}

// EntitySelectorParser.parseNameOrUUID
bool SelectorParser::name_or_uuid(Error& err) {
    const u32 start = r.at;
    std::string word;
    if (!read_string(r, word, err)) return false;
    if (parse_uuid(word, sel.uuid)) {
        sel.by_uuid = true;
        sel.includes_entities = true;
    } else {
        if (word.empty() || word.size() > 16) {
            r.at = start;
            err = here(r, tr("argument.entity.invalid"));
            return false;
        }
        sel.by_name = true;
        sel.name = std::move(word);
        sel.includes_entities = false;
    }
    sel.max_results = 1;
    return true;
}

}  // namespace

// EntityArgument.parse (and GameProfileArgument.parse for Type::kProfile)
bool parse_selector(std::string_view line, u32& cursor, Type type, bool allow_selectors, Selector& out, Error& err) {
    Reader r{line, cursor};
    out = {};
    SelectorParser parser{r, out, {}, {}, {}, {}};
    const bool at_sign = r.can_read() && r.peek() == '@';
    if (at_sign) {
        if (!allow_selectors) {
            err = here(r, tr("argument.entity.selector.not_allowed"));
            return false;
        }
        ++r.at;
        if (!parser.selector(err)) return false;
    } else if (type == Type::kProfile) {  // a name, whatever it is made of, up to the next space
        const u32 start = r.at;
        while (r.can_read() && r.peek() != ' ') ++r.at;
        out.by_name = true;
        out.name = line.substr(start, r.at - start);
        out.max_results = 1;
    } else if (!parser.name_or_uuid(err)) {
        return false;
    }
    const bool single = type == Type::kEntity || type == Type::kPlayer;
    const bool players_only = type == Type::kPlayer || type == Type::kPlayers;
    if (type == Type::kProfile) {
        if (at_sign && out.includes_entities) {
            err = here(r, tr("argument.player.entities"));
            return false;
        }
    } else if (out.max_results > 1 && single) {
        // (The cursor goes back to the start of the line, not of the argument: vanilla's own.)
        err = at(0, tr(players_only ? "argument.player.toomany" : "argument.entity.toomany"));
        return false;
    } else if (out.includes_entities && players_only && !out.current_entity) {
        err = at(0, tr("argument.player.entities"));
        return false;
    }
    cursor = r.at;
    return true;
}

// ---------------------------------------------------------------------------- blocks and items

bool BlockArg::test(world::BlockState s) const {
    const u32 b = world::block_of(s);
    const u32 offset = s - world::block_info(b).first_state;
    if (!is_tag) {
        if (b != block) return false;
        for (const auto& [index, value] : given) {
            const world::BlockProperty p = world::block_property(b, index);
            if (offset / p.stride % p.values.size() != value) return false;
        }
        return true;
    }
    if (std::find(tag_blocks.begin(), tag_blocks.end(), b) == tag_blocks.end()) return false;
    for (const auto& [name, value] : tag_given) {  // TagPredicate: the block has the property, and with this value
        bool matched = false;
        for (u32 i = 0, n = world::block_property_count(b); i < n && !matched; ++i) {
            const world::BlockProperty p = world::block_property(b, i);
            matched = p.name == name && p.values[offset / p.stride % p.values.size()] == value;
        }
        if (!matched) return false;
    }
    return true;
}

bool ItemArg::test(u32 item_id) const {
    if (any) return true;
    if (is_tag) return std::find(tag_items.begin(), tag_items.end(), item_id) != tag_items.end();
    return item_id == item;
}

// BlockStateParser.parse: `predicate` is its forTesting - a tag may stand for the block.
bool parse_block(std::string_view line, u32& cursor, bool predicate, BlockArg& out, Error& err) {
    Reader r{line, cursor};
    out = {};
    std::string id;
    const u32 start = r.at;
    if (r.can_read() && r.peek() == '#') {  // readTag
        if (!predicate) {
            err = here(r, tr("argument.block.tag.disallowed"));
            return false;
        }
        ++r.at;
        if (!read_identifier(r, id, err)) return false;
        out.is_tag = true;
        if (!tag_members("minecraft:block", id, out.tag_blocks)) {
            err = at(start, tr("arguments.block.tag.unknown").arg(id));
            return false;
        }
    } else {  // readBlock
        if (!read_identifier(r, id, err)) return false;
        out.block = world::find_block(id);
        if (out.block == world::kNoId) {
            err = at(start, tr("argument.block.id.invalid").arg(id));
            return false;
        }
        out.state = world::block_info(out.block).default_state;
    }
    if (r.can_read() && r.peek() == '[') {  // readProperties / readVagueProperties
        ++r.at;
        r.skip_whitespace();
        u32 value_start = ~0u;
        while (r.can_read() && r.peek() != ']') {
            r.skip_whitespace();
            const u32 key_start = r.at;
            std::string key, value;
            if (!read_string(r, key, err)) return false;
            const u32 properties = out.is_tag ? 0 : world::block_property_count(out.block);
            u32 index = 0;
            world::BlockProperty property{};
            if (!out.is_tag) {
                for (; index < properties; ++index) {
                    property = world::block_property(out.block, index);
                    if (property.name == key) break;
                }
                if (index == properties) {
                    err = at(key_start, tr("argument.block.property.unknown").arg(id).arg(key));
                    return false;
                }
            }
            const bool duplicate =
                out.is_tag
                    ? std::any_of(out.tag_given.begin(), out.tag_given.end(), [&](const auto& g) { return g.first == key; })
                    : std::any_of(out.given.begin(), out.given.end(), [&](const auto& g) { return g.first == index; });
            if (duplicate) {
                err = at(key_start, tr("argument.block.property.duplicate").arg(key).arg(id));
                return false;
            }
            r.skip_whitespace();
            if (!r.can_read() || r.peek() != '=') {
                // (For a tag vanilla first puts the cursor back on the key.)
                err = at(out.is_tag ? key_start : r.at, tr("argument.block.property.novalue").arg(key).arg(id));
                return false;
            }
            ++r.at;
            r.skip_whitespace();
            value_start = r.at;
            if (!read_string(r, value, err)) return false;
            if (out.is_tag) {
                out.tag_given.emplace_back(std::move(key), std::move(value));
            } else {
                // Property.getValue: the name of a value - or, for a number, anything Integer.parseInt reads as it.
                u32 v = 0;
                for (; v < property.values.size(); ++v) {
                    if (property.values[v] == value) break;
                    i32 want = 0, got = 0;
                    const std::string_view name = property.values[v];
                    const auto a = std::from_chars(name.data(), name.data() + name.size(), want);
                    const auto b = std::from_chars(value.data(), value.data() + value.size(), got);
                    if (a.ec == std::errc() && a.ptr == name.data() + name.size() && b.ec == std::errc() &&
                        b.ptr == value.data() + value.size() && want == got)
                        break;
                }
                if (v == property.values.size()) {
                    err = at(value_start, tr("argument.block.property.invalid").arg(id).arg(value).arg(key));
                    return false;
                }
                // BlockState.setValue: the same state with this one value changed.
                const u32 first = world::block_info(out.block).first_state;
                const u32 now = (out.state - first) / property.stride % static_cast<u32>(property.values.size());
                out.state = static_cast<world::BlockState>(out.state + (static_cast<i32>(v) - static_cast<i32>(now)) *
                                                                           static_cast<i32>(property.stride));
                out.given.emplace_back(static_cast<u16>(index), static_cast<u16>(v));
            }
            r.skip_whitespace();
            if (!r.can_read()) continue;
            value_start = ~0u;
            if (r.peek() == ',') {
                ++r.at;
            } else if (r.peek() != ']') {
                err = here(r, tr("argument.block.property.unclosed"));
                return false;
            }
        }
        if (!r.can_read()) {
            if (out.is_tag && value_start != ~0u) r.at = value_start;
            err = here(r, tr("argument.block.property.unclosed"));
            return false;
        }
        ++r.at;
    }
    if (r.can_read() && r.peek() == '{') {  // readNbt
        err = here(r, unsupported("Block entity data in commands"));
        return false;
    }
    cursor = r.at;
    return true;
}

// ItemParser.parse / ItemPredicateArgument: the item (for a predicate also a tag, or '*').
bool parse_item(std::string_view line, u32& cursor, bool predicate, ItemArg& out, Error& err) {
    Reader r{line, cursor};
    out = {};
    std::string id;
    const u32 start = r.at;
    if (predicate && r.can_read() && r.peek() == '*') {
        ++r.at;
        out.any = true;
    } else if (predicate && r.can_read() && r.peek() == '#') {
        ++r.at;
        if (!read_identifier(r, id, err)) return false;
        out.is_tag = true;
        if (!tag_members("minecraft:item", id, out.tag_items)) {
            err = at(start, tr("arguments.item.tag.unknown").arg(id));
            return false;
        }
    } else {
        if (!read_identifier(r, id, err)) return false;
        out.item = world::find_item(id);
        if (out.item == world::kNoId) {
            err = at(start, tr("argument.item.id.invalid").arg(id));
            return false;
        }
    }
    if (r.can_read() && r.peek() == '[') {
        err = here(r, unsupported(predicate ? "Item predicates with components" : "Item components in commands"));
        return false;
    }
    cursor = r.at;
    return true;
}

}  // namespace server::cmd
