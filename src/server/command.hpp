// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: the tree every "/..." line is read against, and how one is added to it.
//
// The tree and its reading are Brigadier's (com.mojang.brigadier 1.3.11, as vanilla 26.3 ships
// it) - literal and argument nodes, children tried in the order they were added, a literal
// that matches shutting out its argument siblings, the same errors at the same cursor - because
// a client holds a copy of the tree (the `commands` packet) and reads the line against it while
// it is typed: what it shows as valid must be what the server takes.
//
// Adding a command is one line per way of writing it:
//
//     reg.add("give <targets:players> <item:item> [<count:int(1)>]", cmd::kGamemasters, &give);
//     reg.add("tp <targets:entities> <location:vec3> facing entity <facingEntity:entity>", ...);
//     reg.add("fill <from:block_pos> <to:block_pos> <block:block_state> [(outline|hollow)]", ...);
//
//     word                a literal
//     <name:type>         an argument; types below (Type), numbers take bounds: int(1), float(0,2)
//     (a|b|c)             one literal of several, each followed by the rest of the line
//     [ ... ]             optional from here on: the command also runs without what follows.
//                         Brackets nest and close at the end of the line.
//
// Lines that begin alike share their nodes, so the commands of one name are written out as
// vanilla writes its usage; children keep the order in which lines first named them.
//
// A handler is a plain function and a pointer of its own - nothing a script could not hold:
//
//     i32 give(cmd::Invocation& in);   // > 0: how often it worked; 0: it failed (in.fail(...))
//
// It reads what was typed by name (in.integer("count"), in.has("hollow")), asks the game for
// what a name stands for once the line runs (Game::select: "@e[type=item]" is whoever matches
// *now*), and answers through in.success / in.fail. This is the whole surface a plugin needs:
// a Lua plugin's command will be the same registration with one C trampoline as the handler
// and the script's function as `user`.
//
// Everything here is a cold path - a line a person typed, rate-limited per player
// (Game::on_chat_command) - and allocates as it pleases. Tick thread only.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/buf.hpp"
#include "base/types.hpp"
#include "text/text.hpp"
#include "world/block.hpp"

namespace server {

class Game;

namespace cmd {

inline constexpr u32 kNone = ~0u;

// PermissionLevel: what a command asks for, what a source has.
enum Level : u8 { kAll = 0, kModerators = 1, kGamemasters = 2, kAdmins = 3, kOwners = 4 };

// What an argument is read as: the argument types of vanilla that have a parser here.
enum class Type : u8 {
    kBool,            // bool
    kInt,             // int, int(min), int(min,max)
    kFloat,           // float, ...
    kDouble,          // double, ...
    kWord,            // word: up to the next space
    kString,          // string: a word, or "a quoted phrase"
    kGreedy,          // greedy: the rest of the line
    kEntity,          // entity: one entity - a name, a UUID or a selector
    kEntities,        // entities: any number
    kPlayer,          // player: one player
    kPlayers,         // players: any number
    kProfile,         // profile: players by name, online or not (GameProfileArgument)
    kBlockPos,        // block_pos: "1 2 3", "~ ~1 ~", "^ ^ ^2"
    kVec3,            // vec3: the same with fractions; whole numbers mean the middle of the block
    kRotation,        // rotation: yaw and pitch
    kBlockState,      // block_state: stone, oak_stairs[facing=east]
    kBlockPredicate,  // block_predicate: the same, or #tag
    kItem,            // item: stone (components in [] are not read yet)
    kItemPredicate,   // item_predicate: stone, #tag, *
    kIdentifier,      // identifier: namespace:path
    kSound,           // sound: an identifier the client completes from the sounds it has
    kSummonable,      // summonable: an entity type /summon takes
    kMessage,         // message: the rest of the line, as chat
    kAnchor,          // anchor: feet or eyes
};

// "~1": relative to where the source is; "^1": along where it looks (all three or none).
struct Coordinate {
    double value = 0;
    bool relative = false;
};
struct Coordinates {
    Coordinate c[3];     // x y z, or left up forwards - or yaw, pitch
    bool local = false;  // "^ ^ ^"
};

// MinMaxBounds: "3", "1..5", "..5", "2..".
struct Range {
    double min = 0, max = 0;
    bool has_min = false, has_max = false;
    bool matches(double v) const { return !(has_min && min > v) && !(has_max && max < v); }
};

// EntitySelector: what "Steve", a UUID or "@e[type=cow,distance=..5]" came to. Who it names
// is decided when the command runs (Game::select).
struct Selector {
    enum Order : u8 { kArbitrary, kNearest, kFurthest, kRandom };
    struct Test {
        enum Kind : u8 { kName, kType, kTypeTag, kGamemode, kTag, kTeam, kAlive } kind;
        bool inverted = false;
        u32 id = 0;            // kType: the entity type. kGamemode: the GameType id
        std::string text;      // kName, kTag, kTeam
        std::vector<u16> ids;  // kTypeTag: the entity types of the tag
    };
    i32 max_results = 0;
    bool includes_entities = false;  // may name something that is not a player
    bool world_limited = false;
    bool current_entity = false;  // @s
    bool uses_selector = false;   // written with '@'
    Order order = kArbitrary;
    bool by_name = false, by_uuid = false;
    std::string name;
    Uuid uuid;
    u32 type = kNone;  // every match is of this entity type
    bool has_distance = false, has_level = false, has_rot_x = false, has_rot_y = false;
    Range distance, level, rot_x, rot_y;
    bool has_pos[3] = {false, false, false}, has_delta[3] = {false, false, false};
    double pos[3] = {0, 0, 0}, delta[3] = {0, 0, 0};
    std::vector<Test> tests;
};

// BlockStateParser's result: a block and the properties that were spelled out - or, for a
// predicate, possibly a tag.
struct BlockArg {
    u32 block = 0;
    world::BlockState state = 0;             // the block's default state with the given properties set
    std::vector<std::pair<u16, u16>> given;  // (index of the property in the block, index of its value)
    bool is_tag = false;
    std::vector<u16> tag_blocks;                                 // the blocks of the tag
    std::vector<std::pair<std::string, std::string>> tag_given;  // a tag's properties stay words
    // BlockPredicateArgument's predicates: is `s` what was named?
    bool test(world::BlockState s) const;
};

// ItemParser's result, and ItemPredicateArgument's: an item, a tag, or "*".
struct ItemArg {
    u32 item = 0;
    bool any = false, is_tag = false;
    std::vector<u16> tag_items;
    bool test(u32 item_id) const;
};

// MessageArgument.Part: a selector inside a message, and where it stands in the text.
struct MessagePart {
    u32 start = 0, end = 0;
    Selector selector;
};

struct Value {
    Type type{};
    u32 start = 0, end = 0;  // where it stands in the line
    bool flag = false;
    i32 integer = 0;
    double number = 0;
    std::string text;  // a string as read (quotes and escapes resolved), an identifier in full
    Coordinates coords;
    Selector selector;
    BlockArg block;
    ItemArg item;
    std::vector<MessagePart> parts;  // a message's selectors, for a source that may use them
};

// A parser's complaint: CommandSyntaxException. `cursor` is where in the line (createWithContext),
// -1 for one that names no place.
struct Error {
    text::Text message;
    i32 cursor = -1;
    // What a caller may want to tell apart: EntitySelectorParser.ERROR_MISSING_SELECTOR_TYPE and
    // ERROR_UNKNOWN_SELECTOR_TYPE, after which a message takes its '@' as text.
    enum Kind : u8 { kOther, kNoSelectorType } kind = kOther;
};

// Who runs a command: CommandSourceStack.
struct Source {
    u32 player = kNone;          // index of the player; kNone: the console
    u64 entity = 0;              // entity::Handle of that player, 0 for the console
    u8 level = kAll;             // its permission level
    double x = 0, y = 0, z = 0;  // where "~" is
    float yaw = 0, pitch = 0;    // where "^" points
    std::string name;            // getTextName(): the player's name, "Server"
    // CommandSigningContext: the line came as chat_command_signed, so what its message arguments
    // say is said by the player (player_chat) and not for it (disguised_chat).
    bool signs = false;
};

struct Invocation;
// > 0: success, and how much of it; 0: nothing was done - say why with Invocation::fail.
using Handler = i32 (*)(Invocation& in);

// One line being run: the arguments it gave, by the names of the registration.
struct Invocation {
    Game& game;
    const Source& source;
    void* user;              // what the registration passed along
    std::string_view input;  // the whole line, without the slash
    // The nodes the line went through, in order: a literal by its word, an argument by its name.
    struct Step {
        std::string name;
        const Value* value;  // null for a literal
    };
    std::vector<Step> steps;

    bool has(std::string_view name) const { return find(name) != nullptr; }  // a literal or an argument is there
    // The first of "a|b|c" that the line holds, as its index; -1 for none.
    i32 which(std::string_view alternatives) const;
    const Value* value(std::string_view name) const {
        const Step* s = find(name);
        return s ? s->value : nullptr;
    }
    // By name; `or_else` for an optional argument that was left out.
    i32 integer(std::string_view name, i32 or_else = 0) const {
        const Value* v = value(name);
        return v ? v->integer : or_else;
    }
    double number(std::string_view name, double or_else = 0) const {
        const Value* v = value(name);
        return v ? v->number : or_else;
    }
    bool boolean(std::string_view name, bool or_else = false) const {
        const Value* v = value(name);
        return v ? v->flag : or_else;
    }
    std::string_view string(std::string_view name, std::string_view or_else = {}) const {
        const Value* v = value(name);
        return v ? std::string_view(v->text) : or_else;
    }

    // ---- the answer. Both are implemented by the game (server/commands.cpp): it knows who
    // hears what (CommandSourceStack.sendSuccess / sendFailure).
    // `broadcast`: operators and the console are told too ("[Steve: Gave 1 [Stone] to Alex]").
    void success(const text::Text& message, bool broadcast = true);
    // The reason for returning 0. Returns 0, to be returned.
    i32 fail(const text::Text& message);

  private:
    const Step* find(std::string_view name) const {
        for (const Step& s : steps)
            if (s.name == name) return &s;
        return nullptr;
    }
};

// What Registry::parse made of a line.
struct Parsed {
    Handler run = nullptr;  // null: `error` says why not
    void* user = nullptr;
    Error error;
    // The path taken. Values are owned here; `node` indexes Registry's nodes.
    struct Step {
        u32 node;
        bool has_value;
        Value value;
    };
    std::vector<Step> steps;
    u32 last_node = 0;  // the node the reading ended at, the root if none was read
    bool any = false;   // at least one node was read
};

class Registry {
  public:
    Registry();

    // Adds one way of writing a command (the syntax is described at the top). False, with `err`
    // set, for a line that cannot be: a syntax error, an unknown type, a name already taken by
    // an argument of another type.
    bool add(std::string_view syntax, Level level, Handler run, void* user, std::string& err);
    // The same for the server's own commands: a line that cannot be is a bug.
    void add(std::string_view syntax, Level level, Handler run, void* user = nullptr);
    // `name` becomes another word for the command `target` (a node that redirects: "tp").
    bool alias(std::string_view name, std::string_view target, Level level);
    // Takes a command out again, with everything under it (a plugin that is unloaded).
    bool remove(std::string_view name);

    // CommandDispatcher.parse + Commands.finishParsing: reads `line` (no slash) as a source of
    // permission `level` may. Either `run` is set, or `error` is what the source is to be told.
    void parse(std::string_view line, u8 level, Parsed& out) const;

    // The body of the `commands` packet (packet id included) for a source of this level:
    // Commands.sendCommands - only what it may use is in it.
    void encode_tree(u8 level, base::ByteBuf& out) const;
    // Counts every change of the tree: whoever caches encode_tree() compares it.
    u32 version() const { return version_; }

    // CommandDispatcher.getSmartUsage for the node a (partial) line ends at: one line per child.
    // False if the line names no command the source may use.
    bool usage(std::string_view line, u8 level, std::vector<std::string>& out) const;

    std::string_view node_name(u32 node) const { return nodes_[node].name; }

  private:
    struct Node {
        enum Kind : u8 { kRoot, kLiteral, kArgument } kind = kRoot;
        Type type{};
        bool bounded_min = false, bounded_max = false;
        u8 level = kAll;
        double min = 0, max = 0;
        std::string name;
        Handler run = nullptr;
        void* user = nullptr;
        u32 redirect = kNone;
        std::vector<u32> children;
    };
    struct Reading;

    u32 child(u32 parent, const Node& like, std::string& err);
    bool parse_argument(const Node& n, std::string_view line, u32& cursor, u8 level, Value& out, Error& err) const;
    void parse_nodes(u32 node, std::string_view line, u32 cursor, u8 level, const Reading& so_far, Reading& out) const;
    std::string smart_usage(u32 node, u8 level, bool optional, bool deep) const;
    std::string usage_text(u32 node) const;

    std::vector<Node> nodes_;  // [0] is the root; removed nodes stay, unreachable
    u32 version_ = 0;
};

// The line with the error marked, as vanilla shows it under a syntax error: the last ten
// characters before the cursor, what follows underlined, "<--[HERE]".
text::Text error_context(std::string_view line, i32 cursor);

// ---- the parsers by themselves (server/command_args.cpp), for tests and for whoever reads a
// value outside a command. Each reads from line[cursor], moves the cursor, and returns false
// with `err` set as vanilla's parser throws.
bool parse_coordinates(std::string_view line, u32& cursor, Type type, Coordinates& out, Error& err);
bool parse_selector(std::string_view line, u32& cursor, Type type, bool allow_selectors, Selector& out, Error& err);
bool parse_block(std::string_view line, u32& cursor, bool predicate, BlockArg& out, Error& err);
bool parse_item(std::string_view line, u32& cursor, bool predicate, ItemArg& out, Error& err);

// The members of a tag of the registry ("minecraft:block", "#minecraft:logs" without the '#'),
// as the ids the client was sent (update_tags). False if there is no such tag.
bool tag_members(std::string_view registry, std::string_view tag, std::vector<u16>& out);

}  // namespace cmd
}  // namespace server
