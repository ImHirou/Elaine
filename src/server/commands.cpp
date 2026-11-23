// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: who may run them, what running one means here, and the server's own - written
// against the same registration a plugin will use (server/command.hpp).
//
// The rules are vanilla's, read from the 26.3 jar; each function names what it follows:
//   Commands (performCommand, finishParsing, sendCommands), CommandSourceStack (sendSuccess,
//   sendFailure, broadcastToAdmins), ServerGamePacketListenerImpl (handleChatCommand,
//   tryHandleChat, detectRateSpam), PlayerList (op, deop, sendPlayerPermissionLevel),
//   ServerOpListEntry (ops.json), EntitySelector (findEntities, findPlayers),
//   Entity / Player / ItemStack.getDisplayName, CommandResponseTracker,
//   and net.minecraft.server.commands: StopCommand, SetBlockCommand, FillCommand,
//   TeleportCommand, GiveCommand, PlaySoundCommand, StopSoundCommand, SummonCommand, KillCommand,
//   ListPlayersCommand, KickCommand, OpCommand, DeOpCommands, HelpCommand, and the ones that
//   talk: EmoteCommands, MsgCommand, SayCommand, TeamMsgCommand (MessageArgument); TimeCommand.
// What each says, and to whom, was compared with a vanilla 26.3 server
// (tools/vanilla/command_probe.py).
//
// What is NOT here, by command (docs/architecture.md lists it with the reasons):
//   * /setblock, /fill: only blocks the server can simulate (rule 9: the placeable ones, air,
//     and water by itself - a block that holds water is set dry), without block entity data;
//     no particles or sound of a destroyed block;
//   * /tp: no `facing`; "^" coordinates use the processor's sine, not Mth's table;
//   * /give: no components; /summon: no NBT, and what is summoned has no brain - a mob falls
//     to the ground and stands where it was put;
//   * /kill: entities vanish at once (no death animation), and players cannot die yet;
//   * /teammsg: there are no teams, so it answers what vanilla answers someone who is in none;
//   * /op, /deop: a player who is not online is found by name only in offline mode.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include <yyjson.h>

#include "base/java_number.hpp"
#include "base/log.hpp"
#include "crypto/auth.hpp"
#include "item/codec.hpp"
#include "server/game.hpp"
#include "text/nbt_text.hpp"

#include PROTO_GEN_PHYSICS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
using cmd::Invocation;
using text::Color;
using text::Text;
using world::BlockState;

namespace {

constexpr u8 kEventPermissionLevel = 24;       // EntityEvent.PERMISSION_LEVEL_ALL; + the level
constexpr i32 kMaxBlockModifications = 32768;  // GameRules.MAX_BLOCK_MODIFICATIONS
constexpr i32 kGiveMaxStacks = 100;            // GiveCommand.MAX_ALLOWED_ITEMSTACKS
constexpr u8 kCreativeMode = 1;                // GameType.CREATIVE: what everyone is
// net.minecraft.world.entity.Relative, as the bits of player_position.
constexpr u32 kRelX = 1, kRelY = 2, kRelZ = 4, kRelYRot = 8, kRelXRot = 16, kRelDeltaX = 32, kRelDeltaY = 64, kRelDeltaZ = 128;
// SoundSource, by ordinal.
constexpr std::string_view kSoundSources = "master|music|record|weather|block|hostile|neutral|player|ambient|voice|ui";

Text tr(std::string_view key) { return Text::translatable(key); }

// String.equalsIgnoreCase, for names (which are ASCII).
bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
           });
}

// Level.isInSpawnableBounds(BlockPos.containing(x, y, z))
bool spawnable(double x, double y, double z) {
    const i32 bx = floor_int(x), by = floor_int(y), bz = floor_int(z);
    return by >= -20000000 && by < 20000000 && bx >= -30000000 && bz >= -30000000 && bx < 30000000 && bz < 30000000;
}

}  // namespace

// ---------------------------------------------------------------------------- who hears what

void cmd::Invocation::success(const text::Text& message, bool broadcast) { game.command_success(source, message, broadcast); }
i32 cmd::Invocation::fail(const text::Text& message) {
    game.command_failure(source, message);
    return 0;
}

// ServerPlayer.createCommandSourceStack
cmd::Source Game::source_of(u32 pi) const {
    cmd::Source s;
    s.player = pi;
    s.entity = ents_.handle(pi);
    s.level = players_[pi].level;
    s.x = ents_.x[pi], s.y = ents_.y[pi], s.z = ents_.z[pi];
    s.yaw = ents_.yaw[pi], s.pitch = ents_.pitch[pi];
    s.name = players_[pi].name;
    return s;
}

// MinecraftServer.createCommandSourceStack: at the world's spawn, with every permission.
cmd::Source Game::console_source() const {
    cmd::Source s;
    s.level = cmd::kOwners;
    s.x = 8.0, s.y = generator_.spawn_height(), s.z = 8.0;  // Vec3.atLowerCornerOf(the spawn block)
    s.name = "Server";
    return s;
}

// The player a source is, by its handle: a command may have removed players since it began.
u32 Game::player_of(const cmd::Source& source) const {
    if (source.player == cmd::kNone) return kNone;
    const u32 row = ents_.resolve(source.entity);
    return row < players_.size() ? row : kNone;
}

// ServerPlayer.sendSystemMessage: nothing for someone who has the chat hidden, but for what goes
// above the hotbar (acceptsSystemMessages).
void Game::system_message(u32 pi, const Text& message, bool overlay) {
    if (chat_visibility_[pi] == sb::ClientInformation::kChatHidden && !overlay) return;
    text_scratch_.clear();
    message.encode(text_scratch_);
    send(pi, cb::SystemChatComponent{{text_scratch_.data, text_scratch_.len}, overlay});
}

// CommandSourceStack.sendSuccess: the source is told; with `broadcast`, so are the other
// operators and the log (broadcastToAdmins: "[Steve: Set the block at 1, 2, 3]", grey, italic).
void Game::command_success(const cmd::Source& source, const Text& message, bool broadcast) {
    if (source.silent) return;
    const u32 pi = player_of(source);
    if (source.player == cmd::kNone) {
        LOG_INFO("%s", message.plain().c_str());
    } else if (pi != kNone) {
        system_message(pi, message);
    }
    if (!broadcast) return;
    Text admin = tr("chat.type.admin");
    admin.arg(pi != kNone ? display_name(pi) : Text::literal(source.name)).arg(message).color(Color::kGray).italic();
    text_scratch_.clear();
    admin.encode(text_scratch_);
    scratch_.clear();
    write_packet(scratch_, s_.compression_threshold, cb::SystemChatComponent{{text_scratch_.data, text_scratch_.len}});
    for (u32 i = 0; i < players_.size(); ++i)
        if (i != pi && chat_visibility_[i] != sb::ClientInformation::kChatHidden && find_op(players_[i].uuid) != kNone)
            out(i).append(scratch_.data, scratch_.len);
    if (source.player != cmd::kNone) LOG_INFO("%s", admin.plain().c_str());
}

// CommandSourceStack.sendFailure: the message in red.
void Game::command_failure(const cmd::Source& source, const Text& message) {
    if (source.silent) return;
    if (source.player == cmd::kNone) {
        LOG_INFO("%s", message.plain().c_str());
    } else if (const u32 pi = player_of(source); pi != kNone) {
        system_message(pi, Text().append(message).color(Color::kRed));
    }
}

// Entity.getDisplayName: the name with what a click on it does. A player's suggests a whisper;
// anything else shows what it is.
Text Game::display_name(u32 row) const {
    const Uuid uuid = ents_.uuid[row];
    const std::string_view type = world::entity_type_name(ents_.type[row]);
    if (row < players_.size()) {
        const std::string& name = players_[row].name;
        return Text::literal(name)
            .suggest_command("/tell " + name + " ")
            .hover_entity(type, uuid, Text::literal(name))
            .insertion(name);
    }
    // EntityType.getDescription: "entity.minecraft.zombie"; an item entity is named after its stack.
    std::string key;
    if (ents_.type[row] == entity_type::kItem) {
        key = world::item_text_key(ents_.item[row].item());
    } else {
        key = "entity.";
        key += type;
        std::replace(key.begin(), key.end(), ':', '.');
    }
    Text name = tr(key);
    if (ents_.type[row] == entity_type::kFallingBlock) {
        // FallingBlockEntity.getTypeName: "Falling %s", of the block it is (Block.getName).
        std::string block = "block.";
        block += world::block_name(world::block_of(ents_.block_state[row]));
        std::replace(block.begin(), block.end(), ':', '.');
        name = tr("entity.minecraft.falling_block_type").arg(tr(block));
    }
    // Entity.getName: its custom name, if a tag gave it one.
    if (const entity::Cold* c = ents_.cold(row); c && !c->name.empty()) name = Text::encoded(c->name);
    Text shown = name;
    return std::move(shown).hover_entity(type, uuid, std::move(name)).insertion(text::uuid_string(uuid));
}

// ItemStack.getDisplayName: "[Stone]" - its custom name in italics, if it has one - in the
// colour of its rarity, showing the stack when pointed at.
Text Game::item_display_name(const ItemStack& stack) {
    static constexpr Color kRarity[] = {Color::kWhite, Color::kYellow, Color::kAqua, Color::kLightPurple};  // Rarity.color
    namespace component = item::component;
    const item::Patches& store = item::patches();
    const auto patched = [&](u32 type) { return stack.patch() ? store.find(stack.patch(), type) : item::Patches::Found{}; };
    // getHoverName: the custom name - a written book's title is one - or the item's (Item.getName:
    // its ITEM_NAME component, or what its class makes of the stack).
    const auto custom = patched(component::kCustomName), named = patched(component::kItemName);
    const bool has_custom = custom.state == item::Patches::State::kSet;
    const std::string title = has_custom ? std::string() : item::book_title(stack);
    std::string whose;
    const std::string computed = item::computed_name_key(stack, &whose);
    Text name = has_custom                                      ? Text::encoded(custom.value)
                : !title.empty()                                ? Text::literal(title)
                : !computed.empty() && !whose.empty()           ? tr(computed).arg(whose)
                : !computed.empty()                             ? tr(computed)
                : named.state == item::Patches::State::kSet     ? Text::encoded(named.value)
                : named.state == item::Patches::State::kRemoved ? Text()
                                                                : tr(world::item_text_key(stack.item()));
    Text hover = Text().append(std::move(name));
    if (has_custom) hover.italic();
    Text shown = tr("chat.square_brackets").arg(std::move(hover));
    if (stack.empty()) return shown;
    // getRarity: the component, raised by an enchantment.
    u32 rarity = world::item_rarity(stack.item());
    if (const auto r = patched(component::kRarity); r.state == item::Patches::State::kSet) rarity = r.value[0];
    if (stack.enchanted()) rarity = rarity < 2 ? 2 : rarity == 2 ? 3 : rarity;
    shown.color(kRarity[rarity]);
    // (A heavy stack is shown without its components: as a tag they are several times its size,
    // and a line of chat goes to everyone.)
    if (!stack.patch() || item::heavy_bytes(stack))
        return std::move(shown).hover_item(world::item_name(stack.item()), stack.count());
    base::ByteBuf components;
    item::patch_to_wire_nbt(stack.patch(), components);
    return std::move(shown).hover_item(world::item_name(stack.item()), stack.count(), {components.data, components.len});
}

void Game::kick(u32 pi, const Text& reason) {
    Player& p = players_[pi];
    LOG_INFO("%s kicked: %s", p.name.c_str(), reason.plain().c_str());
    text_scratch_.clear();
    reason.encode(text_scratch_);
    send(pi, cb::DisconnectComponent{{text_scratch_.data, text_scratch_.len}});
    base::ByteBuf* b = out_[pi];
    if (p.encrypted) p.enc.encrypt(b->data, b->len);
    b->tag = net::kBufOwnerTick;
    post(p.reactor, {p.slot, p.gen, net::OutKind::SendClose, b});
    out_[pi] = pool_.get(kOutInitial);
    remove_player(pi);
}

// ---------------------------------------------------------------------------- running a line

// Commands.performCommand: read the line as this source may, then run it - or say what is
// wrong with it, and where.
void Game::run_command(const cmd::Source& source, std::string_view line) {
    cmd::Parsed parsed;
    commands_.parse(line, source.level, parsed);
    if (!parsed.run) {
        command_failure(source, parsed.error.message);
        if (parsed.error.cursor >= 0) command_failure(source, cmd::error_context(line, parsed.error.cursor));
        return;
    }
    Invocation in{*this, source, parsed.user, line, {}};
    in.steps.reserve(parsed.steps.size());
    for (const cmd::Parsed::Step& s : parsed.steps)
        in.steps.push_back({std::string(commands_.node_name(s.node)), s.has_value ? &s.value : nullptr});
    parsed.run(in);
}

// handleChatCommand / handleSignedChatCommand -> tryHandleChat, performUnsignedChatCommand /
// performSignedChatCommand, detectCommandRateSpam.
void Game::on_chat_command(u32 pi, const sb::ChatCommand& c) {
    if (c.is_signed && !chat_seen(pi, c.last_seen)) return;
    if (!chat_allowed(c.command)) {
        kick(pi, tr("multiplayer.disconnect.illegal_characters"));
        return;
    }
    const entity::Handle who = ents_.handle(pi);
    // A copy: running it may send large packets, and the command is a view into the inflate buffer.
    const std::string line(c.command);
    LOG_DEBUG("%s: /%s", players_[pi].name.c_str(), line.c_str());
    cmd::Source source = source_of(pi);
    source.signs = c.is_signed;
    // collectSignedArguments: a client with a key signs every message argument of its line and
    // says which - those and no others, or nothing is run. (What it signed is not looked at:
    // without chat sessions SignedMessageChain.Decoder.unsigned takes the text as it is.)
    bool matches = true;
    if (c.signatures) {
        cmd::Parsed parsed;
        commands_.parse(line, source.level, parsed);
        if (parsed.run) {
            u32 named = 0, expected = 0;
            for (const cmd::Parsed::Step& s : parsed.steps) {
                if (!s.has_value || s.value.type != cmd::Type::kMessage) continue;
                ++expected;
                for (u32 i = 0; i < c.signatures; ++i) named += c.signed_names[i] == commands_.node_name(s.node);
            }
            matches = named == expected && c.signatures == expected;
        }
    }
    if (matches)
        run_command(source, line);
    else  // handleMessageDecodeFailure
        system_message(pi, Text().append(tr("chat.disabled.invalid_command_signature")).color(Color::kRed));
    // The command may have removed players - this one among them.
    pi = ents_.resolve(who);
    if (pi < players_.size()) detect_spam(pi, false);
}

// DedicatedServer.handleConsoleInputs: what was typed on stdin since the last tick.
void Game::run_console() {
    if (!console_) return;
    std::string line;
    while (console_->next(line)) {
        std::string_view command = line;
        if (command.starts_with('/')) command.remove_prefix(1);  // Commands.trimOptionalPrefix
        run_command(console_source(), command);
    }
}

// Commands.sendCommands: the tree as far as this player may use it. One encoding per level.
void Game::send_commands(u32 pi) {
    const u8 level = players_[pi].level;
    base::ByteBuf& frame = command_frames_[level];
    if (command_frames_version_[level] != commands_.version()) {
        base::ByteBuf body;
        commands_.encode_tree(level, body);
        frame.clear();
        frame_raw(frame, s_.compression_threshold, {body.data, body.len});
        command_frames_version_[level] = commands_.version();
    }
    out(pi).append(frame.data, frame.len);
}

// PlayerList.sendPlayerPermissionLevel: the client is told its level (it decides what the
// client offers: the game mode switcher, command blocks), then the commands that go with it.
void Game::send_permission_level(u32 pi) {
    send(pi, cb::EntityEvent{ents_.net_id(pi), static_cast<u8>(kEventPermissionLevel + players_[pi].level)});
    send_commands(pi);
}

// ---------------------------------------------------------------------------- operators

u32 Game::find_op(Uuid uuid) const {
    for (u32 i = 0; i < ops_.size(); ++i)
        if (ops_[i].uuid == uuid) return i;
    return kNone;
}

// ops.json, as vanilla keeps it: [{"uuid": ..., "name": ..., "level": 4, "bypassesPlayerLimit": false}].
void Game::load_ops() {
    ops_.clear();
    yyjson_doc* doc = yyjson_read_file(s_.ops_file.c_str(), 0, nullptr, nullptr);
    if (!doc) return;  // no file: no operators
    yyjson_val* entry;
    yyjson_arr_iter it;
    if (yyjson_arr_iter_init(yyjson_doc_get_root(doc), &it)) {
        while ((entry = yyjson_arr_iter_next(&it))) {
            const char* uuid = yyjson_get_str(yyjson_obj_get(entry, "uuid"));
            const char* name = yyjson_get_str(yyjson_obj_get(entry, "name"));
            yyjson_val* level = yyjson_obj_get(entry, "level");
            Op op{};
            unsigned long long a = 0, b = 0, c = 0, d = 0, e = 0;
            if (!uuid || !name || std::sscanf(uuid, "%8llx-%4llx-%4llx-%4llx-%12llx", &a, &b, &c, &d, &e) != 5) continue;
            op.uuid = {a << 32 | b << 16 | c, d << 48 | e};
            op.name = name;
            op.level = static_cast<u8>(std::clamp<i64>(yyjson_is_int(level) ? yyjson_get_sint(level) : 0, 0, cmd::kOwners));
            op.bypasses_player_limit = yyjson_get_bool(yyjson_obj_get(entry, "bypassesPlayerLimit"));
            ops_.push_back(std::move(op));
        }
    }
    yyjson_doc_free(doc);
    LOG_INFO("%zu operator(s) in %s", ops_.size(), s_.ops_file.c_str());
}

void Game::save_ops() const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (const Op& op : ops_) {
        yyjson_mut_val* entry = yyjson_mut_arr_add_obj(doc, root);
        yyjson_mut_obj_add_strcpy(doc, entry, "uuid", text::uuid_string(op.uuid).c_str());
        yyjson_mut_obj_add_strcpy(doc, entry, "name", op.name.c_str());
        yyjson_mut_obj_add_int(doc, entry, "level", op.level);
        yyjson_mut_obj_add_bool(doc, entry, "bypassesPlayerLimit", op.bypasses_player_limit);
    }
    yyjson_write_err err;
    if (!yyjson_mut_write_file(s_.ops_file.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &err))
        LOG_ERROR("cannot write %s: %s", s_.ops_file.c_str(), err.msg);
    yyjson_mut_doc_free(doc);
}

// ---------------------------------------------------------------------------- who a selector names

// EntitySelector.findEntities / findPlayers: the entities, as slots, that `sel` names for this
// source at this moment.
void Game::select(const cmd::Selector& sel, const cmd::Source& source, bool players_only, std::vector<u32>& slots) {
    slots.clear();
    const u32 np = static_cast<u32>(players_.size());
    const bool players = players_only || !sel.includes_entities;
    if (sel.by_name) {  // PlayerList.getPlayerByName: whatever the case
        for (u32 pi = 0; pi < np; ++pi) {
            if (!same_name(players_[pi].name, sel.name)) continue;
            slots.push_back(ents_.slot[pi]);
            return;
        }
        return;
    }
    if (sel.by_uuid) {
        const u32 n = players ? np : ents_.size();
        for (u32 row = 0; row < n; ++row) {
            if (!(ents_.uuid[row] == sel.uuid)) continue;
            slots.push_back(ents_.slot[row]);
            return;
        }
        return;
    }
    // The point distances are taken from: the source's, or the selector's own x, y, z.
    const double at[3] = {sel.has_pos[0] ? sel.pos[0] : source.x, sel.has_pos[1] ? sel.pos[1] : source.y,
                          sel.has_pos[2] ? sel.pos[2] : source.z};
    // EntitySelectorParser.getSelector: dx/dy/dz make a box from that point, a largest distance
    // one around it; an entity is in it if its own box touches it.
    bool boxed = false;
    double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    if (sel.has_delta[0] || sel.has_delta[1] || sel.has_delta[2]) {
        boxed = true;
        for (u32 a = 0; a < 3; ++a) {
            const double d = sel.has_delta[a] ? sel.delta[a] : 0.0;
            lo[a] = at[a] + std::min(d, 0.0);
            hi[a] = at[a] + std::max(d, 0.0) + 1.0;
        }
    } else if (sel.has_distance && sel.distance.has_max) {
        boxed = true;
        for (u32 a = 0; a < 3; ++a) {
            lo[a] = at[a] - sel.distance.max;
            hi[a] = at[a] + sel.distance.max + 1.0;
        }
    }
    const auto in_rotation = [](const cmd::Range& range, float value) {  // createRotationPredicate
        const float min = wrap_degrees(range.has_min ? static_cast<float>(range.min) : 0.0f);
        const float max = wrap_degrees(range.has_max ? static_cast<float>(range.max) : 359.0f);
        const float v = wrap_degrees(value);
        return min > max ? v >= min || v <= max : v >= min && v <= max;
    };
    const auto matches = [&](u32 row) {
        const bool is_player = row < np;
        const u32 type = ents_.type[row];
        if (sel.type != cmd::kNone && type != sel.type) return false;
        for (const cmd::Selector::Test& t : sel.tests) {
            bool is = true;
            switch (t.kind) {
                case cmd::Selector::Test::kName:  // Entity.getPlainTextName
                    is = is_player ? players_[row].name == t.text : display_name(row).plain() == t.text;
                    break;
                case cmd::Selector::Test::kType: is = type == t.id; break;
                case cmd::Selector::Test::kTypeTag: is = std::find(t.ids.begin(), t.ids.end(), type) != t.ids.end(); break;
                case cmd::Selector::Test::kGamemode:  // a predicate only players pass, either way round
                    if (!is_player) return false;
                    is = t.id == kCreativeMode;
                    break;
                // tag=x: it has the tag; tag=: it has none (Entity.getTags, which only a tag of NBT fills).
                case cmd::Selector::Test::kTag: {
                    const entity::Cold* c = ents_.cold(row);
                    is = t.text.empty() ? !c || c->tags.empty()
                                        : c && std::find(c->tags.begin(), c->tags.end(), t.text) != c->tags.end();
                    break;
                }
                // Nothing has a team: an empty name asks for exactly that.
                case cmd::Selector::Test::kTeam: is = t.text.empty(); break;
                case cmd::Selector::Test::kAlive: break;  // whatever is in the table is alive
                case cmd::Selector::Test::kNbt: {         // NbtUtils.compareNbt(tag, what the entity saves as, true)
                    base::ByteBuf saved;
                    nbt::Doc asked, actual;
                    is = save_entity(row, saved) == Saved::kOk &&
                         asked.parse(t.nbt, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
                         actual.parse({saved.data, saved.len}, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
                         nbt::matches(asked.root(), actual.root(), true);
                    break;
                }
            }
            if (is == t.inverted) return false;
        }
        if (sel.has_rot_x && !in_rotation(sel.rot_x, ents_.pitch[row])) return false;
        if (sel.has_rot_y && !in_rotation(sel.rot_y, ents_.yaw[row])) return false;
        if (sel.has_level && !(is_player && sel.level.matches(0))) return false;  // experienceLevel: nobody has any
        const double dx = ents_.x[row] - at[0], dy = ents_.y[row] - at[1], dz = ents_.z[row] - at[2];
        if (boxed) {  // AABB.intersects(entity.getBoundingBox())
            const entity_type::Info& info = entity_type::kInfo[type];
            const double w = static_cast<double>(info.width / 2.0f), h = static_cast<double>(info.height);
            if (!(ents_.x[row] - w < hi[0] && ents_.x[row] + w > lo[0] && ents_.y[row] < hi[1] && ents_.y[row] + h > lo[1] &&
                  ents_.z[row] - w < hi[2] && ents_.z[row] + w > lo[2]))
                return false;
        }
        if (sel.has_distance) {  // MinMaxBounds.Doubles.matchesSqr
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (sel.distance.has_min && sel.distance.min * sel.distance.min > d2) return false;
            if (sel.distance.has_max && sel.distance.max * sel.distance.max < d2) return false;
        }
        return true;
    };
    if (sel.current_entity) {  // @s
        const u32 row = player_of(source);
        if (row != kNone && matches(row)) slots.push_back(ents_.slot[row]);
        return;
    }
    std::vector<u32>& rows = select_scratch_;
    rows.clear();
    // getResultLimit: an unordered search stops when it has enough.
    const u32 limit = sel.order == cmd::Selector::kArbitrary ? static_cast<u32>(sel.max_results) : ~0u;
    for (u32 row = 0, n = players ? np : ents_.size(); row < n && rows.size() < limit; ++row)
        if (matches(row)) rows.push_back(row);
    // sortAndLimit
    if (rows.size() > 1 && sel.order != cmd::Selector::kArbitrary) {
        if (sel.order == cmd::Selector::kRandom) {  // Collections.shuffle
            for (usize i = rows.size(); i > 1; --i) std::swap(rows[i - 1], rows[random_int(static_cast<u32>(i))]);
        } else {
            const auto distance2 = [&](u32 row) {
                const double dx = ents_.x[row] - at[0], dy = ents_.y[row] - at[1], dz = ents_.z[row] - at[2];
                return dx * dx + dy * dy + dz * dz;
            };
            const bool nearest = sel.order == cmd::Selector::kNearest;
            std::stable_sort(rows.begin(), rows.end(),
                             [&](u32 a, u32 b) { return nearest ? distance2(a) < distance2(b) : distance2(a) > distance2(b); });
        }
    }
    if (rows.size() > static_cast<usize>(sel.max_results)) rows.resize(static_cast<usize>(sel.max_results));
    for (const u32 row : rows) slots.push_back(ents_.slot[row]);
}

// ---------------------------------------------------------------------------- what commands do

// Entity.teleportTo(level, x, y, z, relatives, yRot, xRot) as TeleportCommand.performTeleport
// calls it: `x`, `y`, `z` and the rotation are where the entity is to be; `relatives` says
// which of them its client is to take as an offset from where it is (a player keeps moving
// through a "~ ~10 ~"). False - and nothing done - outside the world's bounds.
bool Game::teleport(u32 row, double x, double y, double z, u32 relatives, float yaw, float pitch) {
    if (!spawnable(x, y, z)) return false;
    // What travels: offsets for the relative parts, the rest as they are.
    const double rx = relatives & kRelX ? x - ents_.x[row] : x;
    const double ry = relatives & kRelY ? y - ents_.y[row] : y;
    const double rz = relatives & kRelZ ? z - ents_.z[row] : z;
    const float ryaw = wrap_degrees(relatives & kRelYRot ? yaw - ents_.yaw[row] : yaw);
    const float rpitch = wrap_degrees(relatives & kRelXRot ? pitch - ents_.pitch[row] : pitch);
    // PositionMoveRotation.calculateAbsolute
    const double old_x = ents_.x[row], old_z = ents_.z[row];
    ents_.x[row] = relatives & kRelX ? ents_.x[row] + rx : rx;
    ents_.y[row] = relatives & kRelY ? ents_.y[row] + ry : ry;
    ents_.z[row] = relatives & kRelZ ? ents_.z[row] + rz : rz;
    ents_.yaw[row] = (relatives & kRelYRot ? ents_.yaw[row] : 0.0f) + ryaw;
    ents_.pitch[row] = std::clamp((relatives & kRelXRot ? ents_.pitch[row] : 0.0f) + rpitch, -90.0f, 90.0f);
    if (ents_.flags[row] & entity::flag::kLiving) ents_.head_yaw[row] = ents_.yaw[row];  // setYHeadRot
    // The velocity stays along the axes that moved relatively (calculateDelta: the transition's own is zero).
    ents_.vx[row] = relatives & kRelDeltaX ? ents_.vx[row] + 0.0 : 0.0;
    ents_.vy[row] = relatives & kRelDeltaY ? ents_.vy[row] + 0.0 : 0.0;
    ents_.vz[row] = relatives & kRelDeltaZ ? ents_.vz[row] + 0.0 : 0.0;
    // Entity.sendTeleportTransitionToPlayers: whoever sees an entity that clients move by
    // themselves (an item, a primed TNT), or one that is never updated otherwise, is told at
    // once - with what it is at this point; the others hear of it at its next update.
    if (row >= players_.size()) {
        const u32 type = ents_.type[row];
        if (entity_type::kInfo[type].update_interval == 0 || proto::gen::physics::kEntity[type].simulated) {
            scratch_.clear();
            write_packet(scratch_, s_.compression_threshold,
                         cb::TeleportEntity{ents_.net_id(row), ents_.x[row], ents_.y[row], ents_.z[row], ents_.vx[row],
                                            ents_.vy[row], ents_.vz[row], ents_.yaw[row], ents_.pitch[row], 0,
                                            (ents_.flags[row] & entity::flag::kOnGround) != 0});
            tracker_.broadcast(row, {scratch_.data, scratch_.len}, out_.data());
        }
    }
    // ...then TeleportCommand.performTeleport takes the fall out of it and puts it on the ground.
    ents_.vy[row] = 0;
    ents_.flags[row] |= entity::flag::kOnGround | entity::flag::kMoved;

    if (row < players_.size()) {  // ServerGamePacketListenerImpl.teleport
        Player& p = players_[row];
        if (++p.teleport_id == 0x7FFFFFFF) p.teleport_id = 0;
        p.awaiting_teleport = true;
        send(row, cb::PlayerPosition{p.teleport_id, rx, ry, rz, 0, 0, 0, ryaw, rpitch, relatives});
        apply_view(row, chunk_of(ents_.x[row]), chunk_of(ents_.z[row]), pview_[row]);  // ChunkMap.move
    } else if (const u32 slot = ents_.slot[row]; slot < item_of_slot_.size() && item_of_slot_[slot] != kNone) {
        move_item_cell(slot, old_x, old_z);  // an item lies in the grid cell of where it is
    }
    // Where it stands now: the grid of what can be pushed, and - for what moves by itself -
    // TeleportCommand.performTeleport's setOnGround(true), which looks for the block under it.
    const u32 slot = ents_.slot[row];
    wake(slot);
    {
        const entity_type::Info& info = entity_type::kInfo[ents_.type[row]];
        wake_touching(world::entity_box(ents_.x[row], ents_.y[row], ents_.z[row], info.width, info.height), slot);
    }
    if (slot < movers_.size()) {
        Mover& mv = movers_[slot];
        if (mv.in_grid) move_in_push_grid(slot, ents_.x[row], ents_.z[row]);
        if (mv.kind != proto::gen::physics::kMoverNone) {
            const entity_type::Info& info = entity_type::kInfo[ents_.type[row]];
            phys::World w{world_, phys_scratch_};
            phys::set_on_ground(w, info.width, info.height, motion_of(row), mv.body, true);
        }
    }
    return true;
}

// EntityType.create + Level.addFreshEntity, for what /summon makes: the entity's slot. A
// primed TNT ticks as any other; a living thing falls, slides and is pushed as one that wants
// nothing does (no brain: server/physics.cpp says which types). What nothing here moves stays
// where it is put, and if a client would let it fall by itself, it is told that it does not.
u32 Game::summon(u32 type, double x, double y, double z) {
    namespace gp = proto::gen::physics;
    const Uuid uuid{(random() & ~0xF000ull) | 0x4000ull, (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
    // FallingBlockEntity.DEFAULT_BLOCK_STATE: without a tag to say otherwise it is sand.
    if (type == entity_type::kFallingBlock)
        return add_falling(uuid, static_cast<BlockState>(gen::block::kSand), x, y, z, 0, false);
    entity::Store::Moves moved;
    const u32 row = tracker_.spawn(type, false, uuid, x, y, z, 0, 0, moved);
    const u32 slot = ents_.slot[row];
    const u8 mover = gp::kEntity[type].mover;
    if (type == entity_type::kTnt) {
        ents_.fuse[row] = 80;  // PrimedTnt.DEFAULT_FUSE_TIME: the value clients assume
        tnt_.push_back(slot);
        start_ticking(slot, gp::kMoverTnt);
    } else if (mover == gp::kMoverLiving || mover == gp::kMoverArmorStand) {
        start_ticking(slot, mover);
        enter_push_grid(slot, x, z, entity_type::kInfo[type].width);
        wake_touching(world::entity_box(x, y, z, entity_type::kInfo[type].width, entity_type::kInfo[type].height), slot);
    } else if (type == entity_type::kItem) {
        start_ticking(slot, kMoverEmptyItem);  // an item of nothing: gone with its first tick
    } else if (!entity_type::kInfo[type].living) {
        ents_.set_no_gravity(row, true);
    }
    return slot;
}

// Entity.discard, whatever the entity is.
void Game::remove_entity(u32 slot) {
    if (slot < item_of_slot_.size() && item_of_slot_[slot] != kNone) return remove_item(slot);
    std::erase(tnt_, slot);
    if (slot < falling_.size() && falling_[slot].alive) {
        falling_[slot] = {};
        std::erase(falling_slots_, slot);
    }
    stop_ticking(slot);
    leave_push_grid(slot);
    entity::Store::Moves moved;
    tracker_.despawn(ents_.by_slot(slot), out_.data(), moved);
}

// Level.destroyBlock(pos, true): the block breaks as if mined with an empty hand - it drops
// what its loot table gives, a container spills - and leaves air. False for air.
bool Game::destroy_block_with_drops(BlockPos pos) { return break_block(pos, true); }

// BlockInput.place: the state takes the shape its neighbours give it (unless the change says
// the shape is known), keeps the properties that were spelled out, and is set.
bool Game::place_block_input(BlockPos pos, const cmd::BlockArg& block, u32 flags) {
    BlockState state = flags & kUpdateKnownShape ? block.state : update_from_neighbour_shapes(block.state, pos);
    if (world::is_air(state)) state = block.state;
    // overwriteWithDefinedProperties
    const u32 first = world::block_info(block.block).first_state;
    for (const auto& [index, value] : block.given) {
        const world::BlockProperty p = world::block_property(block.block, index);
        const u32 now = (state - first) / p.stride % static_cast<u32>(p.values.size());
        state = static_cast<BlockState>(state + (static_cast<i32>(value) - static_cast<i32>(now)) * static_cast<i32>(p.stride));
    }
    bool affected = set_block(pos, state, flags);
    if (!block.nbt.empty()) {  // ...and the block entity that is there now is loaded from the tag
        nbt::Doc doc;
        std::string why;
        bool changed = false;
        if (doc.parse(block.nbt, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone)
            load_block_entity(pos, doc.root(), true, changed, why);
        if (changed) world_.block_changed(pos.x, pos.y, pos.z);  // ChunkHolder.blockChanged
        affected |= changed;
    }
    return affected;
}

// ---------------------------------------------------------------------------- the commands

struct Game::Commands {
    // ---- what an argument stands for, now (the getters of vanilla's argument types). Each
    // answers false after telling the source why.

    // EntitySelector.checkPermissions happens at parse time here (Registry::parse_argument).
    static bool entities(Invocation& in, std::string_view name, std::vector<u32>& slots) {  // EntityArgument.getEntities
        in.game.select(in.value(name)->selector, in.source, false, slots);
        if (slots.empty()) in.fail(tr("argument.entity.notfound.entity"));
        return !slots.empty();
    }
    static bool players(Invocation& in, std::string_view name, std::vector<u32>& slots) {  // EntityArgument.getPlayers
        in.game.select(in.value(name)->selector, in.source, true, slots);
        if (slots.empty()) in.fail(tr("argument.entity.notfound.player"));
        return !slots.empty();
    }
    static bool entity(Invocation& in, std::string_view name, u32& slot) {  // EntitySelector.findSingleEntity
        std::vector<u32> slots;
        in.game.select(in.value(name)->selector, in.source, false, slots);
        if (slots.empty()) return in.fail(tr("argument.entity.notfound.entity"));
        if (slots.size() > 1) return in.fail(tr("argument.entity.toomany"));
        slot = slots[0];
        return true;
    }
    // CommandSourceStack.getEntityOrException / getPlayerOrException: the source itself.
    static bool self(Invocation& in, u32& slot, bool must_be_player = false) {
        const u32 pi = in.game.player_of(in.source);
        if (pi == kNone) return in.fail(tr(must_be_player ? "permissions.requires.player" : "permissions.requires.entity"));
        slot = in.game.ents_.slot[pi];
        return true;
    }
    // Coordinates.getPosition: "~" from where the source stands, "^" along where it looks.
    static void position(const Invocation& in, const cmd::Coordinates& c, double out[3]) {
        const cmd::Source& s = in.source;
        if (!c.local) {
            out[0] = c.c[0].relative ? c.c[0].value + s.x : c.c[0].value;
            out[1] = c.c[1].relative ? c.c[1].value + s.y : c.c[1].value;
            out[2] = c.c[2].relative ? c.c[2].value + s.z : c.c[2].value;
            return;
        }
        // LocalCoordinates.apply, in floats as vanilla computes it.
        constexpr float kRad = 0.017453292f;
        const float y_cos = std::cos((s.yaw + 90.0f) * kRad), y_sin = std::sin((s.yaw + 90.0f) * kRad);
        const float x_cos = std::cos(-s.pitch * kRad), x_sin = std::sin(-s.pitch * kRad);
        const float up_cos = std::cos((-s.pitch + 90.0f) * kRad), up_sin = std::sin((-s.pitch + 90.0f) * kRad);
        const double f[3] = {static_cast<double>(y_cos * x_cos), static_cast<double>(x_sin), static_cast<double>(y_sin * x_cos)};
        const double u[3] = {static_cast<double>(y_cos * up_cos), static_cast<double>(up_sin),
                             static_cast<double>(y_sin * up_cos)};
        const double l[3] = {-(f[1] * u[2] - f[2] * u[1]), -(f[2] * u[0] - f[0] * u[2]), -(f[0] * u[1] - f[1] * u[0])};
        const double left = c.c[0].value, up = c.c[1].value, forwards = c.c[2].value;
        const double origin[3] = {s.x, s.y, s.z};
        for (u32 a = 0; a < 3; ++a) out[a] = origin[a] + (f[a] * forwards + u[a] * up + l[a] * left);
    }
    // BlockPosArgument.getLoadedBlockPos
    static bool loaded_block_pos(Invocation& in, std::string_view name, BlockPos& out) {
        double at[3];
        position(in, in.value(name)->coords, at);
        out = {floor_int(at[0]), floor_int(at[1]), floor_int(at[2])};
        Game& g = in.game;
        if (!g.world_.find(out.x >> 4, out.z >> 4)) return in.fail(tr("argument.pos.unloaded"));
        if (out.y < g.world_.min_y() || out.y > g.world_.max_y() || out.x < -30000000 || out.z < -30000000 || out.x >= 30000000 ||
            out.z >= 30000000)
            return in.fail(tr("argument.pos.outofworld"));
        g.world_.ticking(out.x >> 4, out.z >> 4);  // loaded, and now lit and open to changes
        return true;
    }
    // What the world may hold (rule 9): the states of a block whose behaviour is implemented.
    static bool settable(Invocation& in, const cmd::BlockArg& block) {
        const world::BlockInfo& info = world::block_info(block.block);
        const bool known = world::is_air(block.state) ||
                           (info.flags & world::block_flag::kPlaceMask) != world::block_flag::kPlaceNone ||
                           (info.flags & world::block_flag::kSettable);
        // A fluid by itself flows (kSettable: its own block); one in a block that holds it does not yet.
        if (known && (world::state::fluid_legacy_of(block.state) == world::kAir || (info.flags & world::block_flag::kSettable))) {
            // The tag behind it: whatever of it this server cannot do is said before a block is touched.
            nbt::Doc doc;
            std::string what;
            if (block.nbt.empty() || doc.parse(block.nbt, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone ||
                can_load_block_entity(block.block, doc.root(), what))
                return true;
            return in.fail(Text::literal(what + " are not supported by this server yet"));
        }
        std::string why = "The block ";
        why += world::block_name(block.block);
        why += known ? " cannot hold a fluid on this server yet" : " is not supported by this server yet";
        return in.fail(Text::literal(why));
    }
    // BlockPredicateArgument's predicates: the block, and - if a tag was given - a block entity
    // that saves as at least that (NbtUtils.compareNbt).
    static bool block_matches(Game& g, const cmd::BlockArg& filter, BlockPos pos, BlockState state) {
        if (!filter.test(state)) return false;
        if (filter.nbt.empty()) return true;
        nbt::Doc want, has;
        base::ByteBuf saved;
        return g.save_block_entity(pos.x, pos.y, pos.z, saved) == Saved::kOk &&
               want.parse(filter.nbt, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
               has.parse({saved.data, saved.len}, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
               nbt::matches(want.root(), has.root(), true);
    }
    // CommandResponseTracker.sendFeedback for the usual pair of messages: one about the single
    // element, one about how many there were.
    static i32 feedback(Invocation& in, u32 count, Text single, Text multiple) {
        in.success(count == 1 ? single : multiple);
        return static_cast<i32>(count);
    }

    // ---- /stop
    static i32 stop(Invocation& in) {
        in.success(tr("commands.stop.stopping"));
        in.game.stop_requested_ = true;
        return 1;
    }

    // ---- /setblock <pos> <block> [destroy|keep|replace|strict]
    static i32 setblock(Invocation& in) {
        Game& g = in.game;
        BlockPos pos;
        if (!loaded_block_pos(in, "pos", pos)) return 0;
        const cmd::BlockArg& block = in.value("block")->block;
        if (!settable(in, block)) return 0;
        const bool strict = in.has("strict");
        if (in.has("keep") && !world::is_air(g.world_.block(pos.x, pos.y, pos.z))) return in.fail(tr("commands.setblock.failed"));
        bool place = true;
        if (in.has("destroy")) {
            g.destroy_block_with_drops(pos);
            place = !world::is_air(block.state) || !world::is_air(g.world_.block(pos.x, pos.y, pos.z));
        }
        // 2 | 256, or 2 | 816 for `strict`: clients, and never what the old block held.
        const u32 flags = kUpdateClients | kUpdateSkipContainer | (strict ? kUpdateKnownShape | kUpdateSkipOnPlace : 0u);
        if (place && !g.place_block_input(pos, block, flags)) return in.fail(tr("commands.setblock.failed"));
        // ServerLevel.updateNeighboursOnBlockSet: the neighbours hear of the block that is there now.
        if (!strict) g.update_neighbors_at(pos, world::block_of(g.world_.block(pos.x, pos.y, pos.z)));
        in.success(tr("commands.setblock.success").arg(pos.x).arg(pos.y).arg(pos.z));
        return 1;
    }

    // ---- /fill <from> <to> <block> [outline|hollow|destroy|strict|keep|replace [<filter> [...]]]
    static i32 fill(Invocation& in) {
        Game& g = in.game;
        BlockPos a, b;
        if (!loaded_block_pos(in, "from", a) || !loaded_block_pos(in, "to", b)) return 0;
        const cmd::BlockArg& block = in.value("block")->block;
        if (!settable(in, block)) return 0;
        const BlockPos lo{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
        const BlockPos hi{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
        const i64 area = static_cast<i64>(hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
        if (area > kMaxBlockModifications) return in.fail(tr("commands.fill.toobig").arg(kMaxBlockModifications).arg_long(area));
        const cmd::Value* filter = in.value("filter");
        const bool keep = in.has("keep"), strict = in.has("strict"), destroy = in.has("destroy");
        const bool outline = in.has("outline"), hollow = in.has("hollow");
        const u32 flags = kUpdateClients | kUpdateSkipContainer | (strict ? kUpdateKnownShape | kUpdateSkipOnPlace : 0u);
        // Vanilla loads the chunks it walks into; here the region is made ready first.
        for (i32 cz = lo.z >> 4; cz <= hi.z >> 4; ++cz)
            for (i32 cx = lo.x >> 4; cx <= hi.x >> 4; ++cx) g.world_.ticking(cx, cz);
        cmd::BlockArg air;
        air.state = world::kAir;  // FillCommand.HOLLOW_CORE
        std::vector<BlockPos> placed;
        i32 count = 0;
        for (i32 z = lo.z; z <= hi.z; ++z) {  // BlockPos.betweenClosed: x runs fastest, then y, then z
            for (i32 y = lo.y; y <= hi.y; ++y) {
                for (i32 x = lo.x; x <= hi.x; ++x) {
                    const BlockPos pos{x, y, z};
                    const BlockState old = g.world_.block(x, y, z);
                    if (filter && !block_matches(g, filter->block, pos, old)) continue;
                    if (keep && !world::is_air(old)) continue;
                    const bool affected = destroy && g.destroy_block_with_drops(pos);
                    const bool edge = x == lo.x || x == hi.x || y == lo.y || y == hi.y || z == lo.z || z == hi.z;
                    const cmd::BlockArg* what = edge || !(outline || hollow) ? &block : hollow ? &air : nullptr;
                    if (what && g.place_block_input(pos, *what, flags)) {
                        if (!strict) placed.push_back(pos);
                        ++count;
                    } else if (affected) {
                        ++count;
                    }
                }
            }
        }
        for (const BlockPos pos : placed) g.update_neighbors_at(pos, world::block_of(g.world_.block(pos.x, pos.y, pos.z)));
        if (count == 0) return in.fail(tr("commands.fill.failed"));
        in.success(tr("commands.fill.success").arg(count));
        return count;
    }

    // ---- /teleport ... (and /tp)
    static i32 teleport_to_pos(Invocation& in, const std::vector<u32>& slots) {
        Game& g = in.game;
        const cmd::Coordinates& location = in.value("location")->coords;
        const cmd::Value* rotation = in.value("rotation");
        double at[3];
        position(in, location, at);
        // TeleportCommand.getRelatives: what was written with "~" (or "^") moves relatively, and
        // keeps the entity's speed along it; a rotation that was not given is no change.
        u32 relatives = 0;
        for (u32 a = 0; a < 3; ++a)
            if (location.local || location.c[a].relative) relatives |= (kRelX | kRelDeltaX) << a;
        float yaw = 0, pitch = 0;
        if (!rotation) {
            relatives |= kRelYRot | kRelXRot;
        } else {  // Coordinates.getRotation: against the source's own
            const cmd::Coordinate& y = rotation->coords.c[0];
            const cmd::Coordinate& x = rotation->coords.c[1];
            yaw = static_cast<float>(y.relative ? y.value + static_cast<double>(in.source.yaw) : y.value);
            pitch = static_cast<float>(x.relative ? x.value + static_cast<double>(in.source.pitch) : x.value);
            relatives |= (y.relative ? kRelYRot : 0u) | (x.relative ? kRelXRot : 0u);
        }
        Text first;
        for (const u32 slot : slots) {
            const u32 row = g.ents_.by_slot(slot);
            if (!g.teleport(row, at[0], at[1], at[2], relatives, rotation ? yaw : g.ents_.yaw[row],
                            rotation ? pitch : g.ents_.pitch[row]))
                return in.fail(tr("commands.teleport.invalidPosition"));
            if (slot == slots[0]) first = g.display_name(row);
        }
        const std::string x = text::java_format_double(at[0]), y = text::java_format_double(at[1]),
                          z = text::java_format_double(at[2]);
        const u32 n = static_cast<u32>(slots.size());
        return feedback(in, n, tr("commands.teleport.success.location.single").arg(std::move(first)).arg(x).arg(y).arg(z),
                        tr("commands.teleport.success.location.multiple").arg(n).arg(x).arg(y).arg(z));
    }
    static i32 teleport_to_entity(Invocation& in, const std::vector<u32>& slots) {
        Game& g = in.game;
        u32 destination;
        if (!entity(in, "destination", destination)) return 0;
        Text first;
        for (const u32 slot : slots) {
            const u32 to = g.ents_.by_slot(destination), row = g.ents_.by_slot(slot);
            if (!g.teleport(row, g.ents_.x[to], g.ents_.y[to], g.ents_.z[to], 0, g.ents_.yaw[to], g.ents_.pitch[to]))
                return in.fail(tr("commands.teleport.invalidPosition"));
            if (slot == slots[0]) first = g.display_name(row);
        }
        const Text where = g.display_name(g.ents_.by_slot(destination));
        const u32 n = static_cast<u32>(slots.size());
        return feedback(in, n, tr("commands.teleport.success.entity.single").arg(std::move(first)).arg(where),
                        tr("commands.teleport.success.entity.multiple").arg(n).arg(where));
    }
    static i32 teleport_cmd(Invocation& in) {
        std::vector<u32> slots;
        if (in.has("targets")) {
            if (!entities(in, "targets", slots)) return 0;
        } else {
            slots.resize(1);
            if (!self(in, slots[0])) return 0;
        }
        return in.has("destination") ? teleport_to_entity(in, slots) : teleport_to_pos(in, slots);
    }

    // ---- /give <targets> <item> [<count>]
    static i32 give(Invocation& in) {
        Game& g = in.game;
        std::vector<u32> slots;
        if (!players(in, "targets", slots)) return 0;
        const i32 count = in.integer("count", 1);
        // ItemInput.createItemStack(1): the item with the components that were spelled out.
        const ItemStack& prototype = in.value("item")->item.stack;
        // ItemStack.validateStrict (validateComponents).
        if (prototype.damageable_and_stackable())
            return in.fail(tr("arguments.item.malformed").arg("Item cannot be both damageable and stackable"));
        if (u32 held, most; !item::contained_sizes_fit(prototype, held, most))
            return in.fail(tr("arguments.item.malformed")
                               .arg("Item stack with count of " + std::to_string(held) +
                                    " was larger than maximum: " + std::to_string(most)));
        // An empty stack (air) reports the default size, as ItemStack.getMaxStackSize does.
        const i32 max_stack = prototype.empty() ? 64 : static_cast<i32>(prototype.max_stack());
        if (count > max_stack * kGiveMaxStacks)
            return in.fail(
                tr("commands.give.failed.toomanyitems").arg(max_stack * kGiveMaxStacks).arg(item_display_name(prototype)));
        for (const u32 slot : slots) {  // (before anybody is given anything)
            const Player& p = g.players_[g.ents_.by_slot(slot)];
            if (!may_carry(p, prototype, static_cast<u32>((count + max_stack - 1) / max_stack)))
                return in.fail(Text::literal(p.name + " cannot carry that much data in items on this server"));
        }
        Text first;
        for (const u32 slot : slots) {
            const u32 pi = g.ents_.by_slot(slot);
            Player& p = g.players_[pi];
            for (i32 remaining = count; remaining > 0 && !prototype.empty();) {
                const i32 size = std::min(max_stack, remaining);
                remaining -= size;
                ItemStack stack = prototype.with_count(static_cast<u32>(size));
                // Everyone is creative: what does not fit is swallowed, and the stack counts as
                // given - the branch of vanilla's that shows an item flying into the player.
                if (!p.menu.inv.add(-1, stack, true) || !stack.empty()) continue;
                g.fake_item(pi, prototype);
                // SoundEvents.ITEM_PICKUP: ((random - random) * 0.7 + 1) * 2
                g.play_sound(gen::sound::kEntityItemPickup, cb::Sound::kSourcePlayers, g.ents_.x[pi], g.ents_.y[pi],
                             g.ents_.z[pi], 0.2f, ((g.random_float() - g.random_float()) * 0.7f + 1.0f) * 2.0f);
                p.menu.send_changes(g.out(pi), p.threshold, g.containers(p));  // containerMenu.broadcastChanges
                g.dirty_[pi] |= kEquipDirty;
            }
            if (slot == slots[0]) first = g.display_name(pi);
        }
        const u32 n = static_cast<u32>(slots.size());
        return feedback(in, n,
                        tr("commands.give.success.single").arg(count).arg(item_display_name(prototype)).arg(std::move(first)),
                        tr("commands.give.success.multiple").arg(count).arg(item_display_name(prototype)).arg(n));
    }

    // ---- /playsound <sound> [<source> [<targets> [<pos> [<volume> [<pitch> [<minVolume>]]]]]]
    static i32 playsound(Invocation& in) {
        Game& g = in.game;
        const std::string_view sound = in.string("sound");
        const i32 source = std::max(in.which(kSoundSources), 0);  // master, if none was named
        std::vector<u32> slots;
        if (in.has("targets")) {
            if (!players(in, "targets", slots)) return 0;
        } else if (const u32 pi = g.player_of(in.source); pi != kNone) {
            slots.push_back(g.ents_.slot[pi]);
        }
        double at[3] = {in.source.x, in.source.y, in.source.z};
        if (const cmd::Value* pos = in.value("pos")) position(in, pos->coords, at);
        const float volume = static_cast<float>(in.number("volume", 1.0)), pitch = static_cast<float>(in.number("pitch", 1.0));
        const float min_volume = static_cast<float>(in.number("minVolume", 0.0));
        // SoundEvent.getRange: 16 blocks, more for a volume above 1.
        const float range = volume > 1.0f ? 16.0f * volume : 16.0f;
        const double max_distance2 = static_cast<double>(range * range);
        const u64 seed = g.random();
        u32 heard = 0;
        Text first;
        for (const u32 slot : slots) {
            const u32 pi = g.ents_.by_slot(slot);
            const double dx = at[0] - g.ents_.x[pi], dy = at[1] - g.ents_.y[pi], dz = at[2] - g.ents_.z[pi];
            const double distance2 = dx * dx + dy * dy + dz * dz;
            double where[3] = {at[0], at[1], at[2]};
            float local_volume = volume;
            if (distance2 > max_distance2) {
                if (min_volume <= 0.0f) continue;
                // Too far to hear: played two blocks away in its direction, at the least volume.
                const double distance = std::sqrt(distance2);
                where[0] = g.ents_.x[pi] + dx / distance * 2.0;
                where[1] = g.ents_.y[pi] + dy / distance * 2.0;
                where[2] = g.ents_.z[pi] + dz / distance * 2.0;
                local_volume = min_volume;
            }
            g.send(pi, cb::SoundByName{sound, static_cast<u32>(source), where[0], where[1], where[2], local_volume, pitch, seed});
            if (heard++ == 0) first = g.display_name(pi);
        }
        if (heard == 0) return in.fail(tr("commands.playsound.failed"));
        return feedback(in, heard, tr("commands.playsound.success.single").arg(sound).arg(std::move(first)),
                        tr("commands.playsound.success.multiple").arg(sound).arg(heard));
    }

    // ---- /stopsound <targets> [<source>|* [<sound>]]
    static i32 stopsound(Invocation& in) {
        Game& g = in.game;
        std::vector<u32> slots;
        if (!players(in, "targets", slots)) return 0;
        const i32 source = in.which(kSoundSources);
        const std::string_view sound = in.string("sound");
        cb::StopSound packet;
        if (source >= 0) packet.source = static_cast<u32>(source);
        packet.sound = sound;
        for (const u32 slot : slots) g.send(g.ents_.by_slot(slot), packet);
        std::string_view source_name = kSoundSources;
        for (i32 i = 0; i < source; ++i) source_name.remove_prefix(source_name.find('|') + 1);
        source_name = source_name.substr(0, source_name.find('|'));
        if (source >= 0)
            in.success(sound.empty() ? tr("commands.stopsound.success.source.any").arg(source_name)
                                     : tr("commands.stopsound.success.source.sound").arg(sound).arg(source_name));
        else
            in.success(sound.empty() ? tr("commands.stopsound.success.sourceless.any")
                                     : tr("commands.stopsound.success.sourceless.sound").arg(sound));
        return static_cast<i32>(slots.size());
    }

    // ---- /summon <entity> [<pos>]
    static i32 summon(Invocation& in) {
        Game& g = in.game;
        const cmd::Value& what = *in.value("entity");
        const u32 type = static_cast<u32>(what.integer);
        if (!world::summonable(type)) return in.fail(tr("entity.not_summonable").arg(what.text));  // getSummonableEntityType
        double at[3] = {in.source.x, in.source.y, in.source.z};
        if (const cmd::Value* pos = in.value("pos")) position(in, pos->coords, at);
        if (!spawnable(at[0], at[1], at[2])) return in.fail(tr("commands.summon.invalidPosition"));
        nbt::Doc doc;
        nbt::Tag tag{};
        if (const cmd::Value* nbt_value = in.value("nbt");
            nbt_value && doc.parse(nbt_value->tag, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone)
            tag = doc.root();
        if (tag) {  // SummonCommand.createEntity: the entity the tag describes, here
            if (g.ents_.size() >= entity::kMaxEntities) return in.fail(tr("commands.summon.failed"));
            std::string why;
            const u32 slot = g.summon(type, at[0], at[1], at[2], tag, why);
            if (slot == kNone) return in.fail(Text::literal(why + " are not supported by this server yet"));
            in.success(tr("commands.summon.success").arg(g.display_name(g.ents_.by_slot(slot))));
            return 1;
        }
        if (type == entity_type::kItem) {
            // An item entity without a stack: vanilla adds it and its first tick discards it.
            // What is left to see is the answer, naming it after the stack it does not have.
            const Uuid uuid{(g.random() & ~0xF000ull) | 0x4000ull, (g.random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
            const std::string_view air = world::item_text_key(0);
            in.success(tr("commands.summon.success")
                           .arg(tr(air).hover_entity(what.text, uuid, tr(air)).insertion(text::uuid_string(uuid))));
            return 1;
        }
        if (g.ents_.size() >= entity::kMaxEntities) return in.fail(tr("commands.summon.failed"));
        const u32 slot = g.summon(type, at[0], at[1], at[2]);
        in.success(tr("commands.summon.success").arg(g.display_name(g.ents_.by_slot(slot))));
        return 1;
    }

    // ---- /kill [<targets>]
    static i32 kill(Invocation& in) {
        Game& g = in.game;
        std::vector<u32> slots;
        if (in.has("targets")) {
            if (!entities(in, "targets", slots)) return 0;
        } else {
            slots.resize(1);
            if (!self(in, slots[0])) return 0;
        }
        u32 killed = 0;
        Text first;
        for (const u32 slot : slots) {
            const u32 row = g.ents_.by_slot(slot);
            if (row < g.players_.size()) continue;  // nobody can die yet: no health, no respawn
            if (killed++ == 0) first = g.display_name(row);
            g.remove_entity(slot);
        }
        if (killed == 0) return in.fail(Text::literal("Players cannot be killed on this server yet"));
        return feedback(in, killed, tr("commands.kill.success.single").arg(std::move(first)),
                        tr("commands.kill.success.multiple").arg(killed));
    }

    // ---- /list [uuids]
    static i32 list(Invocation& in) {
        Game& g = in.game;
        const bool uuids = in.has("uuids");
        const u32 n = static_cast<u32>(g.players_.size());
        const auto shown = [&](u32 pi) {
            return uuids ? tr("commands.list.nameAndId")
                               .arg(Text::literal(g.players_[pi].name))
                               .arg(text::uuid_string(g.players_[pi].uuid))
                         : g.display_name(pi);
        };
        Text names;  // ComponentUtils.formatList: the one name, or all of them with grey commas between
        if (n == 1) names = shown(0);
        for (u32 pi = 0; pi < n && n > 1; ++pi) {
            if (pi) names.append(Text::literal(", ").color(Color::kGray));
            names.append(shown(pi));
        }
        in.success(tr("commands.list.players").arg(n).arg(g.s_.max_players).arg(std::move(names)), false);
        return static_cast<i32>(n);
    }

    // ---- what is said: /say, /me, /msg (/tell, /w), /teammsg (/tm)
    // MessageArgument.Message.toComponent: the text, with every selector in it replaced by the
    // names of whom it finds - for a source that may use selectors; the others had none read.
    static Text message_component(Invocation& in, std::string_view name) {
        Game& g = in.game;
        const cmd::Value& v = *in.value(name);
        const std::string_view text = v.text;
        if (v.parts.empty()) return Text::literal(text);
        Text out = Text::literal(text.substr(0, v.parts[0].start));
        u32 read_to = v.parts[0].start;
        std::vector<u32> slots;
        for (const cmd::MessagePart& part : v.parts) {
            g.select(part.selector, in.source, false, slots);  // Part.toComponent: EntitySelector.joinNames(findEntities)
            if (read_to < part.start) out.append(text.substr(read_to, part.start - read_to));
            out.append(g.join_names(slots));
            read_to = part.end;
        }
        if (read_to < text.size()) out.append(text.substr(read_to));
        return out;
    }
    // MessageArgument.resolveChatMessage: the player's own words where its client vouched for
    // them (a signed command line), somebody's otherwise - the console's, or a line that came
    // as a plain chat_command.
    static ChatMessage chat_message(Invocation& in, std::string_view name) {
        Game& g = in.game;
        const cmd::Value& v = *in.value(name);
        ChatMessage m;
        const u32 pi = g.player_of(in.source);
        if (in.source.signs && pi != kNone) m.sender = g.players_[pi].uuid;
        m.content = v.text;
        // withUnsignedContent: only what differs from the text as it was typed.
        m.decorated = !v.parts.empty();
        if (m.decorated) m.shown = message_component(in, name);
        return m;
    }
    // CommandSourceStack.getDisplayName
    static Text source_name(const Invocation& in) {
        const u32 pi = in.game.player_of(in.source);
        return pi != kNone ? in.game.display_name(pi) : Text::literal(in.source.name);
    }
    static i32 say(Invocation& in) {
        in.game.broadcast_chat(chat_message(in, "message"), reg::chat_type::kSayCommand, source_name(in));
        return 1;
    }
    static i32 me(Invocation& in) {
        in.game.broadcast_chat(chat_message(in, "action"), reg::chat_type::kEmoteCommand, source_name(in));
        return 1;
    }
    static i32 msg(Invocation& in) {
        Game& g = in.game;
        std::vector<u32> slots;
        if (!players(in, "targets", slots)) return 0;
        const ChatMessage m = chat_message(in, "message");
        const Text name = source_name(in);
        const entity::Handle from = in.source.entity;
        for (const u32 slot : slots) {
            const u32 to = g.ents_.by_slot(slot);
            const Text target = g.display_name(to);
            // CommandSourceStack.sendChatMessage: a player is sent its own words back as
            // "You whisper to ...", the console is told them.
            if (const u32 pi = in.source.player != cmd::kNone ? g.ents_.resolve(from) : kNone; pi < g.players_.size())
                g.send_chat(pi, m, reg::chat_type::kMsgCommandOutgoing, name, &target);
            else if (in.source.player == cmd::kNone)
                LOG_INFO("%s",
                         decorate_chat(reg::chat_type::kMsgCommandOutgoing, name, &target, chat_content(m)).plain().c_str());
            g.send_chat(to, m, reg::chat_type::kMsgCommandIncoming, name, nullptr);
        }
        return static_cast<i32>(slots.size());
    }
    // There are no teams: the source is an entity that is in none, or no entity at all.
    static i32 teammsg(Invocation& in) {
        u32 slot;
        if (!self(in, slot)) return 0;
        return in.fail(tr("commands.teammsg.failed.noteam"));
    }

    // ---- /time (TimeCommand): about a world clock - the one of the source's dimension, or the
    // one named after "of". Every change of a clock goes to everyone at once (modifyClock).
    static bool clock_of(Invocation& in, u32& clock) {
        if (const cmd::Value* named = in.value("clock")) {
            clock = static_cast<u32>(named->integer);
            return true;
        }
        clock = default_clock(reg::dimension_type::kOverworld);  // getDefaultClock: of the source's level - the one there is
        if (clock != ~0u) return true;
        in.fail(tr("commands.time.no_default_clock").arg("minecraft:overworld"));
        return false;
    }
    static i32 wrap_time(i64 ticks) { return static_cast<i32>(ticks % 2147483647ll); }
    static i32 time_set(Invocation& in) {
        Game& g = in.game;
        u32 clock;
        if (!clock_of(in, clock)) return 0;
        Clock& c = g.clocks_[clock];
        const i32 ticks = in.integer("time");
        if (c.total_ticks == ticks) return in.fail(tr("commands.time.set.already_at_time").arg(clock_name(clock)).arg(ticks));
        c.total_ticks = ticks;
        c.partial_tick = 0;
        g.clock_changed(clock);
        in.success(tr("commands.time.set.absolute").arg(clock_name(clock)).arg(ticks));
        return ticks;
    }
    static i32 time_marker(Invocation& in) {
        Game& g = in.game;
        u32 clock;
        if (!clock_of(in, clock)) return 0;
        Clock& c = g.clocks_[clock];
        const std::string_view marker = in.string("timemarker"), name = clock_name(clock);
        const Move moved = move_to_marker(c, clock, marker);
        g.clock_changed(clock);  // (moveToTimeMarker goes through modifyClock whatever comes of it)
        // The two complaints name their two things in opposite orders: vanilla's own.
        if (moved == Move::kNoMarker) return in.fail(tr("commands.time.no_time_marker_found").arg(marker).arg(name));
        if (moved == Move::kNotMoved) return in.fail(tr("commands.time.set.already_at_time_marker").arg(name).arg(marker));
        in.success(tr("commands.time.set.time_marker").arg(name).arg(marker));
        return wrap_time(c.total_ticks);
    }
    static i32 time_add(Invocation& in) {
        Game& g = in.game;
        u32 clock;
        if (!clock_of(in, clock)) return 0;
        Clock& c = g.clocks_[clock];
        c.total_ticks = std::max<i64>(c.total_ticks + in.integer("time"), 0);
        g.clock_changed(clock);
        in.success(tr("commands.time.set.absolute").arg(clock_name(clock)).arg_long(c.total_ticks));
        return wrap_time(c.total_ticks);
    }
    static i32 time_pause(Invocation& in) {  // ...and resume
        Game& g = in.game;
        u32 clock;
        if (!clock_of(in, clock)) return 0;
        Clock& c = g.clocks_[clock];
        const bool paused = in.has("pause");
        if (c.paused == paused)
            return in.fail(
                tr(paused ? "commands.time.pause.already_paused" : "commands.time.pause.already_running").arg(clock_name(clock)));
        c.paused = paused;
        g.clock_changed(clock);
        in.success(tr(paused ? "commands.time.pause" : "commands.time.resume").arg(clock_name(clock)));
        return 1;
    }
    static i32 time_rate(Invocation& in) {
        Game& g = in.game;
        u32 clock;
        if (!clock_of(in, clock)) return 0;
        Clock& c = g.clocks_[clock];
        const float rate = static_cast<float>(in.number("rate"));
        if (c.rate == rate) return in.fail(tr("commands.time.rate.already_same").arg(clock_name(clock)).arg(rate));
        c.rate = rate;
        g.clock_changed(clock);
        in.success(tr("commands.time.rate").arg(clock_name(clock)).arg(rate));
        return 1;
    }
    static i32 time_query(Invocation& in) {
        u32 clock;
        if (!clock_of(in, clock)) return 0;
        const i64 ticks = in.game.clocks_[clock].total_ticks;
        in.success(tr("commands.time.query.absolute").arg(clock_name(clock)).arg_long(ticks), false);
        return wrap_time(ticks);
    }
    // Where a timeline is in its period ("query day": the time of day), or how many periods have
    // passed ("query day repetition": the day).
    static i32 time_query_timeline(Invocation& in) {
        u32 clock;
        if (!clock_of(in, clock)) return 0;
        const u32 timeline = static_cast<u32>(in.integer("timeline"));
        if (timeline_clock(timeline) != clock)
            return in.fail(tr("commands.time.wrong_timeline_for_clock").arg(timeline_name(timeline)).arg(clock_name(clock)));
        const i64 total = in.game.clocks_[clock].total_ticks;
        const bool repetitions = in.has("repetition");
        const i64 answer = repetitions ? timeline_repetitions(timeline, total) : timeline_ticks(timeline, total);
        in.success(tr(repetitions ? "commands.time.query.timeline.repetitions" : "commands.time.query.timeline")
                       .arg(timeline_name(timeline))
                       .arg_long(answer),
                   false);
        return wrap_time(answer);
    }
    static i32 time_query_gametime(Invocation& in) {
        const i64 time = static_cast<i64>(in.game.time_);
        in.success(tr("commands.time.query.gametime").arg_long(time), false);
        return wrap_time(time);
    }

    // ---- /tick query | rate <rate> | step [stop | <time>] | sprint (stop | <time>) | unfreeze | freeze
    // TickCommand. The numbers are strings in vanilla too: String.format("%.1f").
    static std::string ns_as_ms(u64 ns) {  // nanosToMilisString
        return base::java_format_fixed(static_cast<double>(static_cast<float>(ns) / 1e6f), 1);
    }
    static i32 tick_query(Invocation& in) {
        Game& g = in.game;
        const TickRate& rate = g.rate_;
        const u64 average = g.average_tick_ns();
        const std::string busy = ns_as_ms(average);
        const std::string per_second = base::java_format_fixed(static_cast<double>(rate.rate), 1);
        if (rate.sprinting()) {
            in.success(tr("commands.tick.status.sprinting"), false);
            in.success(tr("commands.tick.query.rate.sprinting").arg(per_second).arg(busy), false);
        } else {
            in.success(tr(rate.frozen                  ? "commands.tick.status.frozen"
                          : rate.ns_per_tick < average ? "commands.tick.status.lagging"
                                                       : "commands.tick.status.running"),
                       false);
            in.success(tr("commands.tick.query.rate.running").arg(per_second).arg(busy).arg(ns_as_ms(rate.ns_per_tick)), false);
        }
        u64 samples[kTickSamples];
        std::copy(std::begin(g.tick_times_), std::end(g.tick_times_), samples);
        std::sort(std::begin(samples), std::end(samples));
        in.success(tr("commands.tick.query.percentiles")
                       .arg(ns_as_ms(samples[kTickSamples / 2]))
                       .arg(ns_as_ms(samples[static_cast<u32>(kTickSamples * 0.95)]))
                       .arg(ns_as_ms(samples[static_cast<u32>(kTickSamples * 0.99)]))
                       .arg(static_cast<i32>(kTickSamples)),
                   false);
        return static_cast<i32>(rate.rate);
    }
    static i32 tick_rate(Invocation& in) {
        const float rate = static_cast<float>(in.number("rate"));
        in.game.set_tick_rate(rate);
        in.success(tr("commands.tick.rate.success").arg(base::java_format_fixed(static_cast<double>(rate), 1)));
        return static_cast<i32>(rate);
    }
    static i32 tick_step(Invocation& in) {
        const i32 advance = in.integer("time", 1);
        if (!in.game.step_game_if_paused(advance)) return in.fail(tr("commands.tick.step.fail"));
        in.success(tr("commands.tick.step.success").arg(advance));
        return 1;
    }
    static i32 tick_step_stop(Invocation& in) {
        if (!in.game.stop_stepping()) return in.fail(tr("commands.tick.step.stop.fail"));
        in.success(tr("commands.tick.step.stop.success"));
        return 1;
    }
    static i32 tick_sprint(Invocation& in) {
        if (in.game.request_game_to_sprint(in.integer("time"))) in.success(tr("commands.tick.sprint.stop.success"));
        in.success(tr("commands.tick.status.sprinting"));
        return 1;
    }
    static i32 tick_sprint_stop(Invocation& in) {
        if (!in.game.stop_sprinting()) return in.fail(tr("commands.tick.sprint.stop.fail"));
        in.success(tr("commands.tick.sprint.stop.success"));
        return 1;
    }
    static i32 tick_freeze(Invocation& in) {  // ...and unfreeze
        Game& g = in.game;
        const bool freeze = in.has("freeze");
        if (freeze) {
            if (g.rate_.sprinting()) g.stop_sprinting();
            if (g.rate_.stepping_forward()) g.stop_stepping();
        }
        g.set_frozen(freeze);
        in.success(tr(freeze ? "commands.tick.status.frozen" : "commands.tick.status.running"));
        return freeze ? 1 : 0;
    }

    // ---- /kick <targets> [<reason>]
    // ---- /data get (entity <target> | block <target> | storage <target>) [<path> [<scale>]]
    // DataCommands, the reading third of it: what something saves as, a tag in it, a number in
    // it times a scale.
    static i32 data_get(Invocation& in) {
        Game& g = in.game;
        base::ByteBuf saved;
        // The answers name what was asked - "commands.data.<kind>.query" and ".get" - and begin
        // with it: an entity by its name, a block by its three coordinates, a storage by its id.
        std::string_view kind;
        Text entity_name;
        BlockPos pos{};
        if (in.has("entity")) {
            u32 slot;
            if (!entity(in, "target", slot)) return 0;
            const u32 row = g.ents_.by_slot(slot);
            g.save_entity(row, saved);
            kind = "entity";
            entity_name = g.display_name(row);
        } else if (in.has("block")) {
            if (!loaded_block_pos(in, "target", pos)) return 0;
            if (g.save_block_entity(pos.x, pos.y, pos.z, saved) == Saved::kNone)
                return in.fail(tr("commands.data.block.invalid"));
            kind = "block";
        } else {  // CommandStorage.get: of a storage nobody wrote to, an empty compound - and nothing writes one here
            const u8 empty[] = {nbt::kCompound, nbt::kEnd};
            saved.append(empty, sizeof empty);
            kind = "storage";
        }
        nbt::Doc doc;
        if (doc.parse({saved.data, saved.len}, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone)
            return in.fail(Text::literal("The saved tag could not be read"));
        const auto answer = [&](std::string_view what) {
            std::string key = "commands.data.";
            key += kind;
            key += '.';
            key += what;
            return tr(key);
        };
        const auto subject = [&](Text& t) {
            if (kind == "entity") {
                t.arg(entity_name);
            } else if (kind == "block") {
                t.arg(pos.x).arg(pos.y).arg(pos.z);
            } else {
                t.arg(in.string("target"));
            }
        };
        // NbtUtils.toPrettyComponent of a tag one message can carry.
        const auto shown = [&](nbt::Tag tag, Text& t) {
            constexpr usize kMaxShown = 65536;
            if (tag.bytes().size() > kMaxShown) return false;
            t.arg(text::pretty_nbt(tag));
            return true;
        };
        const cmd::Value* path = in.value("path");
        if (!path) {
            Text t = answer("query");
            subject(t);
            if (!shown(doc.root(), t)) return in.fail(Text::literal("The tag is too large to show"));
            in.success(t, false);
            return 1;
        }
        // getSingleTag
        std::vector<nbt::Tag> found;
        std::string_view missing;
        if (!nbt::find(path->path, doc.root(), found, missing))
            return in.fail(tr("arguments.nbtpath.nothing_found").arg(missing));
        if (found.size() > 1) return in.fail(tr("commands.data.get.multiple"));
        const nbt::Tag tag = found[0];
        if (const cmd::Value* scale = in.value("scale")) {  // getNumeric
            if (!tag.numeric()) return in.fail(tr("commands.data.get.invalid").arg(path->path.text));
            const i32 result = floor_int(tag.as_double() * scale->number);
            Text t = answer("get");
            t.arg(path->path.text);
            subject(t);
            t.arg(base::java_format_fixed(scale->number, 2)).arg(result);
            in.success(t, false);
            return result;
        }
        // getData: a number, floored; how many a list, an array or a compound holds; how long a string is.
        const i32 result = tag.numeric()              ? floor_int(tag.as_double())
                           : tag.type == nbt::kString ? static_cast<i32>(nbt::utf16_length(tag.str()))
                                                      : static_cast<i32>(tag.size());
        Text t = answer("query");
        subject(t);
        if (!shown(tag, t)) return in.fail(Text::literal("The tag is too large to show"));
        in.success(t, false);
        return result;
    }

    // ---- /tellraw <targets> <message>
    // (ComponentUtils.resolve fills in what a component asks the server for - selectors, scores,
    // NBT; a component that holds such is not read here, so there is nothing to fill in.)
    static i32 tellraw(Invocation& in) {
        Game& g = in.game;
        std::vector<u32> slots;
        if (!players(in, "targets", slots)) return 0;
        const std::vector<u8>& message = in.value("message")->tag;
        for (const u32 slot : slots) {
            const u32 pi = g.ents_.by_slot(slot);
            if (g.chat_visibility_[pi] != sb::ClientInformation::kChatHidden)  // ServerPlayer.sendSystemMessage
                g.send(pi, cb::SystemChatComponent{{message.data(), static_cast<u32>(message.size())}, false});
        }
        return static_cast<i32>(slots.size());
    }

    static i32 kick(Invocation& in) {
        Game& g = in.game;
        std::vector<u32> slots;
        if (!players(in, "targets", slots)) return 0;
        const Text reason = in.has("reason") ? message_component(in, "reason") : tr("multiplayer.disconnect.kicked");
        for (const u32 slot : slots) {
            const u32 pi = g.ents_.by_slot(slot);
            const Text who = g.display_name(pi);
            g.kick(pi, reason);
            in.success(tr("commands.kick.success").arg(who).arg(reason));
        }
        return static_cast<i32>(slots.size());
    }

    // ---- /op <targets>, /deop <targets>
    struct Profile {
        Uuid uuid;
        std::string name;
    };
    // GameProfileArgument.getGameProfiles: players by a selector, or one profile by its name.
    static bool profiles(Invocation& in, std::string_view name, std::vector<Profile>& out) {
        Game& g = in.game;
        const cmd::Selector& sel = in.value(name)->selector;
        if (sel.uses_selector) {
            std::vector<u32> slots;
            if (!players(in, name, slots)) return false;
            for (const u32 slot : slots)
                out.push_back({g.players_[g.ents_.by_slot(slot)].uuid, g.players_[g.ents_.by_slot(slot)].name});
            return true;
        }
        // The profile cache knows names without their case.
        for (const Player& p : g.players_) {
            if (!same_name(p.name, sel.name)) continue;
            out.push_back({p.uuid, p.name});
            return true;
        }
        for (const Op& op : g.ops_) {
            if (!same_name(op.name, sel.name)) continue;
            out.push_back({op.uuid, op.name});
            return true;
        }
        // Someone who is not here: in offline mode a name is its own profile; in online mode
        // Mojang would have to be asked, which this server does not do yet.
        if (g.s_.online_mode || sel.name.empty()) return in.fail(tr("argument.player.unknown"));
        out.push_back({crypto::offline_uuid(sel.name), sel.name});
        return true;
    }
    static i32 set_op(Invocation& in, bool op) {
        Game& g = in.game;
        std::vector<Profile> who;
        if (!profiles(in, "targets", who)) return 0;
        i32 count = 0;
        for (const Profile& profile : who) {
            const u32 at = g.find_op(profile.uuid);
            if (op == (at != kNone)) continue;
            if (op)
                g.ops_.push_back({profile.uuid, profile.name, static_cast<u8>(g.s_.op_permission_level), false});
            else
                g.ops_.erase(g.ops_.begin() + at);
            g.save_ops();
            for (u32 pi = 0; pi < g.players_.size(); ++pi) {  // PlayerList.sendPlayerPermissionLevel
                if (!(g.players_[pi].uuid == profile.uuid)) continue;
                g.players_[pi].level = op ? static_cast<u8>(g.s_.op_permission_level) : static_cast<u8>(cmd::kAll);
                g.send_permission_level(pi);
            }
            ++count;
            in.success(tr(op ? "commands.op.success" : "commands.deop.success").arg(profile.name));
        }
        if (count == 0) return in.fail(tr(op ? "commands.op.failed" : "commands.deop.failed"));
        return count;
    }
    static i32 op(Invocation& in) { return set_op(in, true); }
    static i32 deop(Invocation& in) { return set_op(in, false); }

    // ---- /help [<command>]
    static i32 help(Invocation& in) {
        const std::string_view about = in.string("command");
        std::vector<std::string> lines;
        if (!in.game.commands_.usage(about, in.source.level, lines)) return in.fail(tr("commands.help.failed"));
        for (const std::string& line : lines) {
            std::string shown("/");
            if (!about.empty()) {
                shown += about;
                shown += ' ';
            }
            shown += line;
            in.success(Text::literal(shown), false);
        }
        return static_cast<i32>(lines.size());
    }
};

// The commands in vanilla's own words: the nodes, their names and their order are those of
// the classes in net.minecraft.server.commands (tools/vanilla/command_probe.py compares the
// tree with reports/commands.json).
void Game::init_commands() {
    using C = Commands;
    cmd::Registry& r = commands_;
    const std::string sources = "(" + std::string(kSoundSources) + ")";

    // DataCommands: of merge, get, modify and remove, the one that only reads. Its targets in
    // the order of ALL_PROVIDERS.
    for (const std::string_view target : {"entity <target:entity>", "block <target:block_pos>", "storage <target:storage>"})
        r.add("data get " + std::string(target) + " [<path:nbt_path> [<scale:double>]]", cmd::kGamemasters, &C::data_get);
    r.add("me <action:message>", cmd::kAll, &C::me);
    r.add("fill <from:block_pos> <to:block_pos> <block:block_state> [(outline|hollow|destroy|strict)]", cmd::kGamemasters,
          &C::fill);
    r.add(
        "fill <from:block_pos> <to:block_pos> <block:block_state> replace [<filter:block_predicate> "
        "[(outline|hollow|destroy|strict)]]",
        cmd::kGamemasters, &C::fill);
    r.add("fill <from:block_pos> <to:block_pos> <block:block_state> keep", cmd::kGamemasters, &C::fill);
    r.add("give <targets:players> <item:item> [<count:int(1)>]", cmd::kGamemasters, &C::give);
    r.add("help [<command:greedy>]", cmd::kAll, &C::help);
    r.add("kick <targets:players> [<reason:message>]", cmd::kAdmins, &C::kick);
    r.add("kill [<targets:entities>]", cmd::kGamemasters, &C::kill);
    r.add("list [uuids]", cmd::kAll, &C::list);
    r.add("msg <targets:players> <message:message>", cmd::kAll, &C::msg);
    r.alias("tell", "msg", cmd::kAll);
    r.alias("w", "msg", cmd::kAll);
    r.add("playsound <sound:sound> [" + sources +
              " [<targets:players> [<pos:vec3> [<volume:float(0)> [<pitch:float(0,2)> "
              "[<minVolume:float(0,1)>]]]]]]",
          cmd::kGamemasters, &C::playsound);
    r.add("say <message:message>", cmd::kGamemasters, &C::say);
    r.add("setblock <pos:block_pos> <block:block_state> [(destroy|keep|replace|strict)]", cmd::kGamemasters, &C::setblock);
    r.add("stopsound <targets:players> [* <sound:sound>]", cmd::kGamemasters, &C::stopsound);
    r.add("stopsound <targets:players> " + sources + " [<sound:sound>]", cmd::kGamemasters, &C::stopsound);
    r.add("summon <entity:summonable> [<pos:vec3> [<nbt:nbt_compound>]]", cmd::kGamemasters, &C::summon);
    r.add("teammsg <message:message>", cmd::kAll, &C::teammsg);
    r.alias("tm", "teammsg", cmd::kAll);
    r.add("teleport <location:vec3>", cmd::kGamemasters, &C::teleport_cmd);
    r.add("teleport <destination:entity>", cmd::kGamemasters, &C::teleport_cmd);
    r.add("teleport <targets:entities> <location:vec3> [<rotation:rotation>]", cmd::kGamemasters, &C::teleport_cmd);
    r.add("teleport <targets:entities> <destination:entity>", cmd::kGamemasters, &C::teleport_cmd);
    r.alias("tp", "teleport", cmd::kGamemasters);
    r.add("tellraw <targets:players> <message:component>", cmd::kGamemasters, &C::tellraw);
    // TickCommand: what it suggests for a rate and a time is its own (`.suggests`), not the types'.
    r.add("tick query", cmd::kAdmins, &C::tick_query);
    r.add("tick rate <rate:float(1,10000)=20>", cmd::kAdmins, &C::tick_rate);
    r.add("tick step", cmd::kAdmins, &C::tick_step);
    r.add("tick step stop", cmd::kAdmins, &C::tick_step_stop);
    r.add("tick step <time:time(1)=1t|1s>", cmd::kAdmins, &C::tick_step);
    r.add("tick sprint stop", cmd::kAdmins, &C::tick_sprint_stop);
    r.add("tick sprint <time:time(1)=60s|1d|3d>", cmd::kAdmins, &C::tick_sprint);
    r.add("tick unfreeze", cmd::kAdmins, &C::tick_freeze);
    r.add("tick freeze", cmd::kAdmins, &C::tick_freeze);
    // TimeCommand: the same nodes under "time" and under "time of <clock>" (addClockNodes), in
    // that order; "query gametime" is the level's and stands under the first alone.
    for (const std::string& at : {std::string("time"), std::string("time of <clock:clock>")}) {
        r.add(at + " set <time:time>", cmd::kGamemasters, &C::time_set);
        r.add(at + " set <timemarker:time_marker>", cmd::kGamemasters, &C::time_marker);
        r.add(at + " add <time:time(-2147483648)>", cmd::kGamemasters, &C::time_add);
        r.add(at + " pause", cmd::kGamemasters, &C::time_pause);
        r.add(at + " resume", cmd::kGamemasters, &C::time_pause);
        r.add(at + " rate <rate:float(0.00001,1000)>", cmd::kGamemasters, &C::time_rate);
        r.add(at + " query time", cmd::kGamemasters, &C::time_query);
        r.add(at + " query <timeline:timeline> [repetition]", cmd::kGamemasters, &C::time_query_timeline);
        if (at == "time") r.add("time query gametime", cmd::kGamemasters, &C::time_query_gametime);
    }
    r.add("deop <targets:profile>", cmd::kAdmins, &C::deop);
    r.add("op <targets:profile>", cmd::kAdmins, &C::op);
    r.add("stop", cmd::kOwners, &C::stop);
    commands_sent_ = commands_.version();
}

}  // namespace server
