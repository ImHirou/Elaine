// SPDX-License-Identifier: GPL-3.0-or-later
// Signs: standing, on a wall, hanging from a ceiling, hanging between walls (26.3:
// StandingSignBlock, WallSignBlock, CeilingHangingSignBlock, WallHangingSignBlock, SignBlock;
// SignBlockEntity, SignText; StandingAndWallBlockItem, HangingSignItem; the SignApplicators -
// DyeItem, InkSacItem, GlowInkSacItem, HoneycombItem).
//
// A sign is a block entity whose two texts are kept as SignText's StreamCodec writes them - the
// form the item components `sign_text_front` / `sign_text_back` have, with the codecs that go
// with it (item/codec.hpp): four lines, each a text component as a tag, perhaps four filtered
// ones, a dye, whether it glows. What a client is shown of it is its update tag
// (SignBlockEntity.getUpdateTag = saveCustomOnly), which lies with its chunk
// (world::Dimension::set_block_entity_tag): the chunk's packet carries it, and a change sends
// block_entity_data behind the block's update (Game::broadcast_block_changes).
//
// Writing: the block that is placed opens the editor of its front (setPlacedBy), a click with
// an empty hand that of the side the player stands on; whoever was sent the editor is the one
// player whose sign_update is taken (playerWhoMayEdit), until it answers or walks away. A line
// keeps the style it had; what a client sends is plain text.
//
// Where this is not vanilla's: Mth.atan2 is a table there and the library's here, so a player
// standing exactly beside a sign may be on the other side of it; a text with a selector or a
// score is not read at all (text/component.hpp), so `allow_op_features` never resolves one.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "base/log.hpp"
#include "item/codec.hpp"
#include "nbt/nbt.hpp"
#include "server/game.hpp"
#include "text/component.hpp"
#include "text/text.hpp"

#include PROTO_GEN_ITEMS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
using world::BlockState;
namespace block_kind = proto::gen::block_kind;
namespace props = proto::gen::props;
namespace comp = item::component;

namespace {

constexpr u8 kBlack = 15;                          // DyeColor.BLACK: the colour of a text nobody dyed
constexpr u8 kEmptyLine[] = {nbt::kString, 0, 0};  // CommonComponents.EMPTY, as a tag

// SignText, taken apart: SignText.STREAM_CODEC is four tags, perhaps four more, a dye, a flag.
struct Side {
    std::vector<u8> line[4], filtered[4];
    bool has_filtered = false;
    u8 color = kBlack;
    bool glowing = false;
};

std::vector<u8> take_tag(Reader& r) {
    nbt::Doc doc;
    if (doc.parse({r.p, r.remaining()}, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone) {
        r.ok = false;
        return {std::begin(kEmptyLine), std::end(kEmptyLine)};
    }
    const std::span<const u8> bytes = r.bytes(doc.size());
    return {bytes.begin(), bytes.end()};
}

Side read_side(std::span<const u8> wire) {
    Side s;
    if (wire.empty()) {  // SignText.EMPTY
        for (std::vector<u8>& line : s.line) line.assign(std::begin(kEmptyLine), std::end(kEmptyLine));
        return s;
    }
    Reader r(wire);
    for (std::vector<u8>& line : s.line) line = take_tag(r);
    s.has_filtered = r.u8v() != 0;
    if (s.has_filtered)
        for (std::vector<u8>& line : s.filtered) line = take_tag(r);
    s.color = static_cast<u8>(r.varint());
    s.glowing = r.u8v() != 0;
    return s;
}

void write_side(const Side& s, std::vector<u8>& out) {
    out.clear();
    for (const std::vector<u8>& line : s.line) out.insert(out.end(), line.begin(), line.end());
    out.push_back(s.has_filtered);
    if (s.has_filtered)
        for (const std::vector<u8>& line : s.filtered) out.insert(out.end(), line.begin(), line.end());
    out.push_back(s.color);  // (16 colours: one byte of a VarInt)
    out.push_back(s.glowing);
}

nbt::Tag root_of(const std::vector<u8>& tag, nbt::Doc& doc) {
    return doc.parse(tag, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone ? doc.root() : nbt::Tag{};
}

// SignText.hasEditableText: every line is plain text (PlainTextContents) - a literal, styled or
// not; not a translation, a keybind, whatever else a text can be.
bool editable(const Side& s) {
    return std::all_of(std::begin(s.line), std::end(s.line), [](const std::vector<u8>& line) {
        nbt::Doc doc;
        const nbt::Tag t = root_of(line, doc);
        return t.type == nbt::kString || (t.type == nbt::kCompound && t.get("text"));
    });
}

// SignText.hasMessage: a line that is not the empty string, as text.
bool has_message(const Side& s) {
    return std::any_of(std::begin(s.line), std::end(s.line), [](const std::vector<u8>& line) {
        nbt::Doc doc;
        const nbt::Tag t = root_of(line, doc);
        if (t.type == nbt::kString) return !t.str().empty();
        const nbt::Tag text = t.get("text");
        return t.type == nbt::kCompound && (!text || !text.str().empty() || t.get("extra"));
    });
}

// The click event of a line that a click on the sign sets off (executeClickCommandsIfPresent):
// run_command, show_dialog, custom. `command`: a run_command's line.
bool click_action(const std::vector<u8>& line, std::string& command) {
    nbt::Doc doc;
    const nbt::Tag event = root_of(line, doc).get("click_event");
    if (event.type != nbt::kCompound || event.get("action").type != nbt::kString) return false;
    const std::string_view action = event.get("action").str();
    command.clear();
    if (action == "run_command" && event.get("command").type == nbt::kString) command = nbt::to_utf8(event.get("command").str());
    return action == "run_command" || action == "show_dialog" || action == "custom";
}

// ChatFormatting.stripFormatting: a section sign and the letter behind it go ("(?i)§[0-9A-FK-OR]").
std::string strip_formatting(std::string_view s) {
    std::string out;
    for (usize i = 0; i < s.size(); ++i) {
        if (i + 2 < s.size() && static_cast<u8>(s[i]) == 0xC2 && static_cast<u8>(s[i + 1]) == 0xA7) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + 2])));
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'k' && c <= 'o') || c == 'r') {
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Component.literal(text).setStyle(the style `old` has): the new line of a sign. A line without
// a style is a string; one with a style keeps everything about it that is not its content.
std::vector<u8> with_style_of(const std::vector<u8>& old, std::string_view text) {
    base::ByteBuf built, out;
    nbt::Doc doc;
    const nbt::Tag was = root_of(old, doc);
    nbt::Writer w(built);
    if (was.type != nbt::kCompound) {
        w.root(nbt::kString);
        w.put_string(text);
    } else {
        w.root(nbt::kCompound);
        w.put_string("text", text);
        static constexpr std::string_view kContent[] = {"text",    "extra",  "type",     "translate", "fallback", "with",
                                                        "keybind", "score",  "selector", "separator", "nbt",      "interpret",
                                                        "block",   "entity", "storage",  "source",    "object",   "atlas",
                                                        "sprite",  "player", "hat"};
        for (const nbt::Tag e : was.entries())
            if (std::find(std::begin(kContent), std::end(kContent), e.name()) == std::end(kContent))
                w.put_raw(e.name(), e.type, e.bytes());
        w.end_compound();
    }
    nbt::Doc again;
    if (again.parse({built.data, built.len}, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone ||
        text::transcode(again.root(), out) != text::Result::kOk)
        return {std::begin(kEmptyLine), std::end(kEmptyLine)};
    return {out.data, out.data + out.len};
}

// SegmentedAnglePrecision(4): sixteenths of a turn. Math.round, and what does not fit wraps.
u32 segment_of_degrees(float degrees) {
    return static_cast<u32>(static_cast<i32>(std::floor(degrees * (16.0f / 360.0f) + 0.5f))) & 15u;
}
float degrees_of_segment(u32 segment) {
    const float degrees = static_cast<float>(segment & 15u) * (360.0f / 16.0f);
    return degrees >= 180.0f ? degrees - 360.0f : degrees;
}
// Direction.get2DDataValue: south 0, west 1, north 2, east 3 - and back.
constexpr u32 kFrom2D[4] = {world::kSouth, world::kWest, world::kNorth, world::kEast};
u32 data_2d(u32 dir) { return dir == world::kSouth ? 0 : dir == world::kWest ? 1 : dir == world::kNorth ? 2 : 3; }
// Direction.fromYRot
u32 direction_of_yaw(float yaw) {
    return kFrom2D[static_cast<u32>(static_cast<i32>(std::floor(static_cast<double>(yaw) / 90.0 + 0.5))) & 3u];
}
bool same_axis(u32 a, u32 b) { return (a >> 1) == (b >> 1); }
BlockPos step(BlockPos p, u32 dir) { return {p.x + world::kStepX[dir], p.y + world::kStepY[dir], p.z + world::kStepZ[dir]}; }
u32 clockwise(u32 dir) {
    return dir == world::kNorth   ? world::kEast
           : dir == world::kEast  ? world::kSouth
           : dir == world::kSouth ? world::kWest
                                  : world::kNorth;
}

}  // namespace

// ---------------------------------------------------------------------------- the block entity

bool Game::sign_kind(u32 kind) { return kind >= block_kind::kSign && kind <= block_kind::kWallHangingSign; }

u32 Game::new_sign(BlockPos pos) {
    u32 data;
    if (!sign_free_.empty()) {
        data = sign_free_.back();
        sign_free_.pop_back();
    } else {
        data = static_cast<u32>(signs_.size());
        signs_.emplace_back();
    }
    signs_[data] = {};
    signs_[data].alive = true;
    signs_[data].x = pos.x, signs_[data].y = pos.y, signs_[data].z = pos.z;
    return data;
}

// A sign's state, in the properties its four classes have: which way it faces (as a Direction),
// the sixteenth of a turn it stands at, whether it hangs by its middle.
u32 Game::sign_facing(BlockState s) {
    const world::BlockInfo& info = world::block_info_of(s);
    return world::kNorth + (s - info.first_state) / props::wall_sign::kFacingStride % 4;
}
u32 Game::sign_rotation(BlockState s) {
    const world::BlockInfo& info = world::block_info_of(s);
    return (s - info.first_state) / props::sign::kRotationStride % 16;  // (the hanging sign's stride is the same)
}

// SignBlockEntity.saveCustomOnly: what a client is shown of it. Its entries as saveAdditional puts them.
void Game::write_sign_tag(const Sign& sign, nbt::Writer& w) const {
    static_assert(props::sign::kRotationStride == props::hanging_sign::kRotationStride);
    if (sign.allow_op) w.put_byte("allow_op_features", 1);
    const u8 empty[] = {kEmptyLine[0],
                        kEmptyLine[1],
                        kEmptyLine[2],
                        kEmptyLine[0],
                        kEmptyLine[1],
                        kEmptyLine[2],
                        kEmptyLine[0],
                        kEmptyLine[1],
                        kEmptyLine[2],
                        kEmptyLine[0],
                        kEmptyLine[1],
                        kEmptyLine[2],
                        0,
                        kBlack,
                        0};
    for (u32 side = 0; side < 2; ++side) {
        const std::span<const u8> text =
            sign.text[side].empty() ? std::span<const u8>(empty) : std::span<const u8>(sign.text[side]);
        item::value_to_nbt(comp::kSignTextFront, text, w, side == 0 ? "front_text" : "back_text");
    }
    w.put_byte("is_waxed", sign.waxed);
}

// What a client is shown of it goes to its chunk: the chunk's packet carries it from now on.
void Game::show_sign(u32 data) {
    const Sign& sign = signs_[data];
    base::ByteBuf tag;
    nbt::Writer w(tag);
    w.root(nbt::kCompound);
    write_sign_tag(sign, w);
    w.end_compound();
    world_.set_block_entity_tag(sign.x, sign.y, sign.z, {tag.data, tag.len});
}

// SignBlockEntity.markUpdated: it is saved again, and those who see it are told - the block,
// and behind it the block entity's data.
void Game::sign_changed(u32 data) {
    show_sign(data);
    world_.block_changed(signs_[data].x, signs_[data].y, signs_[data].z);
    world_.mark_dirty(signs_[data].x, signs_[data].z);
}

// SignBlockEntity.getSlotPlayerIsFacing: the front, if the player stands within a quarter turn
// of where the sign looks.
bool Game::sign_front_faces(u32 pi, const Sign& sign) const {
    const BlockState state = world_.block(sign.x, sign.y, sign.z);
    const u32 kind = world::block_info_of(state).kind;
    double cx = 0.5, cz = 0.5;  // SignBlock.getSignHitboxCenterPosition: of a wall sign, the middle of its board
    float sign_yaw;
    if (kind == block_kind::kWallSign || kind == block_kind::kWallHangingSign) {
        const u32 facing = sign_facing(state);
        sign_yaw = static_cast<float>(data_2d(facing) * 90);  // Direction.toYRot
        if (kind == block_kind::kWallSign) {                  // Block.boxZ(16, 4.5, 12.5, 14, 16), turned
            cx = facing == world::kWest ? 0.9375 : facing == world::kEast ? 0.0625 : 0.5;
            cz = facing == world::kNorth ? 0.9375 : facing == world::kSouth ? 0.0625 : 0.5;
        }
    } else {
        sign_yaw = degrees_of_segment(sign_rotation(state));
    }
    const double xd = ents_.x[pi] - (static_cast<double>(sign.x) + cx), zd = ents_.z[pi] - (static_cast<double>(sign.z) + cz);
    const float player_yaw = static_cast<float>(std::atan2(zd, xd) * 57.2957763671875) - 90.0f;
    // Mth.degreesDifferenceAbs: the angle between the two, wrapped.
    float diff = std::fmod(player_yaw - sign_yaw, 360.0f);
    if (diff >= 180.0f) diff -= 360.0f;
    if (diff < -180.0f) diff += 360.0f;
    return std::fabs(diff) <= 90.0f;
}

// SignBlock.openTextEdit: this player may write on it now, and is sent the editor.
void Game::open_sign_editor(u32 pi, u32 data, bool front) {
    Sign& sign = signs_[data];
    sign.editor = players_[pi].uuid;
    if (!sign.editing) editing_signs_.push_back(data);
    sign.editing = true;
    // ServerPlayer.openTextEdit: the block first, so that the client has the sign it is to edit.
    send(pi, cb::BlockUpdate{sign.x, sign.y, sign.z, world_.block(sign.x, sign.y, sign.z)});
    send(pi, cb::OpenSignEditor{sign.x, sign.y, sign.z, front});
}

// SignBlockEntity.tick: whoever may write on it and has gone, or has walked more than four
// blocks beyond its reach, may not any more.
void Game::tick_signs() {
    for (usize i = 0; i < editing_signs_.size();) {
        Sign& sign = signs_[editing_signs_[i]];
        bool still = sign.alive && sign.editing;
        if (still) {
            still = false;
            for (u32 pi = 0; pi < players_.size(); ++pi)
                if (players_[pi].uuid == sign.editor) still = within_reach(pi, {sign.x, sign.y, sign.z}, 4.0);
        }
        if (still) {
            ++i;
            continue;
        }
        if (sign.alive) sign.editing = false;
        editing_signs_[i] = editing_signs_.back();
        editing_signs_.pop_back();
    }
}

// SignBlock.setPlacedBy: whoever placed it writes on its front, unless it came with a text that
// cannot be edited or with wax on it.
void Game::sign_placed(u32 pi, BlockPos pos) {
    const world::BlockEntityRef* ref = world_.block_entity(pos.x, pos.y, pos.z);
    if (!ref || (ref->data & kOpaqueBlockEntity) || !sign_kind(world::block_info_of(world_.block(pos.x, pos.y, pos.z)).kind))
        return;
    const Sign& sign = signs_[ref->data];
    if (!sign.waxed && editable(read_side(sign.text[0]))) open_sign_editor(pi, ref->data, true);
}

// handleSignUpdate -> SignBlockEntity.updateSignText: the four lines a client wrote.
void Game::on_sign_update(u32 pi, const sb::SignUpdate& u) {
    const world::BlockEntityRef* ref = world_.block_entity(u.pos.x, u.pos.y, u.pos.z);
    if (!ref || (ref->data & kOpaqueBlockEntity) ||
        !sign_kind(world::block_info_of(world_.block(u.pos.x, u.pos.y, u.pos.z)).kind))
        return;
    Sign& sign = signs_[ref->data];
    if (sign.waxed || !sign.editing || !(sign.editor == players_[pi].uuid)) {
        LOG_DEBUG("%s just tried to change a sign it may not edit", players_[pi].name.c_str());
        return;
    }
    // updateMessages: each line a literal of what was sent, in the style the line had. (Nothing
    // is filtered here, so the filtered text is the text.)
    const u32 side = u.front ? 0 : 1;
    Side text = read_side(sign.text[side]);
    for (u32 i = 0; i < 4; ++i) {
        text.line[i] = with_style_of(text.line[i], strip_formatting(u.lines[i]));
        text.filtered[i].clear();
    }
    text.has_filtered = false;
    write_side(text, sign.text[side]);
    sign.editing = false;  // setAllowedPlayerEditor(null); tick_signs drops it from its list
    sign_changed(ref->data);
}

// SignBlock.useItemOn, and - for the main hand - useWithoutItem behind it.
Game::Used Game::use_sign(u32 pi, BlockPos pos, BlockState state, const ItemStack& held, u32 hand) {
    namespace it = proto::gen::item;
    const world::BlockEntityRef* ref = world_.block_entity(pos.x, pos.y, pos.z);
    if (!ref || (ref->data & kOpaqueBlockEntity)) return Used::kPass;
    const u32 data = ref->data, kind = world::block_info_of(state).kind;
    Sign& sign = signs_[data];
    Player& p = players_[pi];
    const bool front = sign_front_faces(pi, sign);
    const u32 side = front ? 0 : 1;
    const bool others = sign.editing && !(sign.editor == p.uuid);  // otherPlayerIsEditingSign

    // executeClickCommandsIfPresent: what the lines of that side do when clicked - if the sign
    // may (allow_op_features), else a line over the hotbar saying that it may not.
    const auto click_commands = [&] {
        const Side text = read_side(sign.text[side]);
        bool any = false;
        std::string command;
        for (const std::vector<u8>& line : text.line) {
            if (!click_action(line, command)) continue;
            any = true;
            if (!sign.allow_op || command.empty()) continue;
            // createCommandSourceStack: the player, at the middle of the sign, as a game master, unheard.
            cmd::Source source = source_of(pi);
            source.level = cmd::kGamemasters;
            source.x = pos.x + 0.5, source.y = pos.y + 0.5, source.z = pos.z + 0.5;
            source.yaw = source.pitch = 0;
            source.silent = true;
            run_command(source, command.starts_with('/') ? std::string_view(command).substr(1) : std::string_view(command));
        }
        if (any && !sign.allow_op)
            system_message(pi, text::Text::translatable("sign.click_actions_disabled").color(text::Color::kRed), true);
        return any;
    };
    const auto clickable = [&] {  // hasAnyClickCommands
        const Side text = read_side(sign.text[side]);
        std::string command;
        return std::any_of(std::begin(text.line), std::end(text.line),
                           [&](const std::vector<u8>& line) { return click_action(line, command) && !command.empty(); });
    };

    // A hanging sign under which another is being hung lets the item have the click
    // (shouldTryToChainAnotherHangingSign).
    const auto hangs = [](u32 k) { return k == block_kind::kHangingSign || k == block_kind::kWallHangingSign; };
    const bool hanging_item =
        hangs(kind) && !held.empty() && held.info().block != gen::kNoBlock && hangs(world::block_info(held.info().block).kind);
    if (hanging_item && !(sign.waxed && clickable())) {
        if (kind == block_kind::kHangingSign && last_use_face_ == world::kDown) return Used::kPass;
        if (kind == block_kind::kWallHangingSign && !same_axis(last_use_face_, sign_facing(state))) return Used::kPass;
    }

    // ---- useItemOn: a SignApplicator in the hand - a dye, an ink sac, a glow ink sac, a honeycomb
    const u32 item = held.empty() ? 0 : held.item();
    const item::Patches::Found dye = held.empty() ? item::Patches::Found{} : item::effective(held, comp::kDye);
    const bool dye_item = !held.empty() && item::default_of(item, comp::kDye).has;  // DyeItem
    const bool applicator = dye_item || item == it::kInkSac || item == it::kGlowInkSac || item == it::kHoneycomb;
    if (applicator && !sign.waxed && !others) {
        Side text = read_side(sign.text[side]);
        bool applied = false;
        if (item == it::kHoneycomb) {  // canApplyToSign: always. setWaxed(true)
            sign.waxed = true;
            applied = true;
            level_event(3003, pos, 0);  // LevelEvent.PARTICLES_AND_SOUND_WAX_ON
            play_sound(gen::sound::kItemHoneycombWaxOn, cb::Sound::kSourceBlocks, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 1.0f,
                       1.0f);
        } else if (has_message(text)) {  // SignApplicator.canApplyToSign: there is something written
            if (dye_item) {
                if (dye.state == item::Patches::State::kSet && dye.value[0] != text.color) {  // SignText.withColor
                    text.color = dye.value[0];
                    applied = true;
                    play_sound(gen::sound::kItemDyeUse, cb::Sound::kSourceBlocks, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 1.0f,
                               1.0f);
                }
            } else if (text.glowing != (item == it::kGlowInkSac)) {  // withGlowingText
                text.glowing = item == it::kGlowInkSac;
                applied = true;
                play_sound(item == it::kGlowInkSac ? gen::sound::kItemGlowInkSacUse : gen::sound::kItemInkSacUse,
                           cb::Sound::kSourceBlocks, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 1.0f, 1.0f);
            }
            if (applied) write_side(text, sign.text[side]);
        }
        if (applied) {
            sign_changed(data);
            click_commands();
            return Used::kSuccess;  // (itemStack.consume: nothing is taken from a creative player)
        }
    }
    // ---- TRY_WITH_EMPTY_HAND: useWithoutItem, which is the main hand's
    if (hand != cb::SwingAnimation::kMainHand) return Used::kPass;
    const bool ran = click_commands();
    if (sign.waxed) {
        play_sound(gen::sound::kBlockSignWaxedInteractFail, cb::Sound::kSourceBlocks, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 1.0f,
                   1.0f);
        return Used::kSuccessServer;
    }
    if (ran) return Used::kSuccessServer;
    if (!others && editable(read_side(sign.text[side]))) {
        open_sign_editor(pi, data, front);
        return Used::kSuccessServer;
    }
    return Used::kPass;
}

// ---------------------------------------------------------------------------- placing

// The state a sign's item places: StandingSignBlock / WallSignBlock.getStateForPlacement under
// StandingAndWallBlockItem.getPlacementState (which hangs nothing from above), or
// CeilingHangingSignBlock / WallHangingSignBlock.getStateForPlacement under HangingSignItem's
// (which stands nothing on the ground).
BlockState Game::sign_for_placement(u32 pi, const sb::UseItemOn& u, const world::BlockInfo& block, u32 kind, BlockPos at,
                                    bool replacing_clicked) const {
    const float yaw = ents_.yaw[pi];
    const u32 wet = world::holds_water_source(state_for_client(at)) ? 0 : 1;  // waterlogged: true, false
    const u32 standing_first = world::block_info_of(block.alt[0]).first_state, wall_first = block.alt[1];
    u32 dirs[6];
    placement_directions(pi, u.direction, replacing_clicked, dirs);
    const auto none = world::Section::kNoState;

    if (kind == world::block_flag::kPlaceSign) {
        const BlockState standing =
            static_cast<BlockState>(standing_first + segment_of_degrees(yaw + 180.0f) * props::sign::kRotationStride + wet);
        BlockState wall = none;  // on the first wall the player looks at that holds it
        for (const u32 dir : dirs) {
            if (dir < world::kNorth) continue;
            const BlockState s = static_cast<BlockState>(
                wall_first + (world::opposite(dir) - world::kNorth) * props::wall_sign::kFacingStride + wet);
            if (can_survive(s, at)) {
                wall = s;
                break;
            }
        }
        for (const u32 dir : dirs) {
            if (dir == world::kUp) continue;
            const BlockState possible = dir == world::kDown ? standing : wall;
            if (possible != none && can_survive(possible, at)) return possible;
        }
        return none;
    }

    // ---- a hanging sign. From a ceiling: by both chains if what is above has a whole face
    // down and the player does not sneak, else by the middle; under another hanging sign that
    // runs the same way, in line with it.
    const BlockState above = state_for_client({at.x, at.y + 1, at.z});
    const u32 above_kind = world::block_info_of(above).kind;
    const u32 looks = direction_of_yaw(yaw);
    const bool sneaks = players_[pi].shift;
    bool middle = !world::collision_full_below(above) || sneaks;
    if (!sneaks && above_kind == block_kind::kWallHangingSign) {
        if (same_axis(sign_facing(above), looks)) middle = false;
    } else if (!sneaks && above_kind == block_kind::kHangingSign) {
        const u32 rotation = sign_rotation(above);  // RotationSegment.convertToDirection: only the four straight ones
        const u32 straight = rotation == 0   ? world::kNorth
                             : rotation == 4 ? world::kEast
                             : rotation == 8 ? world::kSouth
                                             : world::kWest;
        if (rotation % 4 == 0 && same_axis(straight, looks)) middle = false;
    }
    const u32 segment = middle ? segment_of_degrees(yaw + 180.0f) : data_2d(world::opposite(looks)) << 2;
    const BlockState ceiling = static_cast<BlockState>(standing_first + (middle ? 0 : 1) * props::hanging_sign::kAttachedStride +
                                                       segment * props::hanging_sign::kRotationStride + wet);
    // Between walls: across the way the player looks, where a wall - or a sign that hangs the
    // same way - is on one side of it (WallHangingSignBlock.canPlace).
    const auto attaches = [&](BlockState s, BlockPos to, u32 face) {
        const BlockState other = state_for_client(to);
        if (world::block_info_of(other).kind == block_kind::kWallHangingSign)
            return same_axis(sign_facing(other), sign_facing(s));
        return world::face_sturdy(other, face);
    };
    BlockState wall = none;
    for (const u32 dir : dirs) {
        if (dir < world::kNorth || same_axis(dir, u.direction)) continue;
        const u32 facing = world::opposite(dir);
        const BlockState s =
            static_cast<BlockState>(wall_first + (facing - world::kNorth) * props::wall_sign::kFacingStride + wet);
        const u32 cw = clockwise(facing), ccw = world::opposite(cw);
        if (attaches(s, step(at, cw), ccw) || attaches(s, step(at, ccw), cw)) {
            wall = s;
            break;
        }
    }
    for (const u32 dir : dirs) {  // HangingSignItem: attachmentDirection UP
        if (dir == world::kDown) continue;
        const BlockState possible = dir == world::kUp ? ceiling : wall;
        if (possible != none && can_survive(possible, at)) return possible;
    }
    return none;
}

}  // namespace server
