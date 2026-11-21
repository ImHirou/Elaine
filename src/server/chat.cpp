// SPDX-License-Identifier: GPL-3.0-or-later
// Chat: what players say, to whom it goes, and in which packet.
//
// The rules are vanilla's, read from the 26.3 jar; each function names what it follows:
//   ServerGamePacketListenerImpl (handleChat, tryHandleChat, unpackAndApplyLastSeen,
//   handleChatAck, handleChatSessionUpdate, sendPlayerChatMessage, detectRateSpam),
//   LastSeenMessagesValidator, SignedMessageChain.Decoder.unsigned, PlayerChatMessage,
//   OutgoingChatMessage, PlayerList (broadcastChatMessage, broadcastSystemMessage),
//   ServerPlayer (sendChatMessage, sendSystemMessage), ChatType.Bound, MinecraftServer.logChatMessage.
//
// There is no secure chat here: no player has a chat session, because nothing here can check
// the key a client offers for one (that takes Mojang's Services keys). Vanilla has that case
// too - a server that could not fetch those keys, or one in offline mode - and this is that
// server: enforcesSecureChat is false in `login`, chat_session_update is ignored, every message
// is PlayerChatMessage.unsigned - player_chat with no signature, index 0, the server's own time,
// salt 0 and an empty list of what its sender had seen. A client shows such a message with a
// "Not Secure" tag, and not at all when it is set to show secure chat only.
//
// One message goes to many: everything of player_chat but its first field - a count the server
// keeps for each recipient - is encoded once (PlayerChatTail) and copied behind that count.
#include <chrono>

#include "base/log.hpp"
#include "server/game.hpp"

#include PROTO_GEN_CHAT_TYPES_HPP

namespace server {

using namespace proto;
using text::Color;
using text::Text;

namespace {

constexpr u32 kSpamStep = 20;  // TickThrottler(20, 20 * <chat|command>-spam-threshold-seconds)
constexpr u8 kFull = sb::ClientInformation::kChatFull, kHidden = sb::ClientInformation::kChatHidden;

Text tr(std::string_view key) { return Text::translatable(key); }

i64 now_ms() {  // Instant.now().toEpochMilli()
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

bool chat_allowed(std::string_view s) {
    for (usize i = 0; i < s.size(); ++i) {
        const u8 c = static_cast<u8>(s[i]);
        if (c < 0x20 || c == 0x7F) return false;
        if (c == 0xC2 && i + 1 < s.size() && static_cast<u8>(s[i + 1]) == 0xA7) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------- what comes in

// unpackAndApplyLastSeen: LastSeenMessagesValidator.applyUpdate, of a validator that never has
// anything in it. A client acknowledges the signed messages it was sent; none ever is, so its
// window is twenty empty places and stays where it is: an offset would move it past its end, an
// acknowledged place acknowledges nothing, and the checksum of nothing is 1 (0: not checked).
// Anything else is a client that is out of step, and it is put out.
bool Game::chat_seen(u32 pi, const sb::LastSeenUpdate& seen) {
    if (LIKELY(seen.offset == 0 && seen.acknowledged == 0 && seen.checksum <= 1)) return true;
    LOG_WARN("failed to validate message acknowledgements from %s", players_[pi].name.c_str());
    kick(pi, tr("multiplayer.disconnect.chat_validation_failed"));
    return false;
}

// handleChatAck: LastSeenMessagesValidator.applyOffset - the same window, moved between messages.
void Game::on_chat_ack(u32 pi, u32 offset) {
    if (offset == 0) return;
    LOG_WARN("failed to validate message acknowledgement offset from %s", players_[pi].name.c_str());
    kick(pi, tr("multiplayer.disconnect.chat_validation_failed"));
}

// TickThrottler.increment + isUnderThreshold, caught up to this tick, and detectRateSpam: whoever
// is over is put out, unless an operator. True: the player is gone.
bool Game::detect_spam(u32 pi, bool chat) {
    Player& p = players_[pi];
    u32& count = chat ? p.chat_spam : p.command_spam;
    u64& as_of = chat ? p.chat_spam_tick : p.command_spam_tick;
    count -= static_cast<u32>(std::min<u64>(count, tick_ - as_of));  // TickThrottler.tick: one less a tick
    as_of = tick_;
    count += kSpamStep;
    if (count < kSpamStep * (chat ? s_.chat_spam_threshold : s_.command_spam_threshold) || find_op(p.uuid) != kNone) return false;
    kick(pi, tr("disconnect.spam"));
    return true;
}

// handleChat: what the sender has seen first, then tryHandleChat - the characters, whether the
// sender shows chat at all - then the message itself (SignedMessageChain.Decoder.unsigned: the
// signature a client with a key sends along is not looked at) to everyone, and the rate.
void Game::on_chat(u32 pi, const sb::Chat& c) {
    if (!chat_seen(pi, c.last_seen)) return;
    if (!chat_allowed(c.message)) {
        kick(pi, tr("multiplayer.disconnect.illegal_characters"));
        return;
    }
    if (chat_visibility_[pi] == kHidden) {  // (straight to the connection: its system messages are hidden too)
        text_scratch_.clear();
        tr("chat.disabled.options").color(Color::kRed).encode(text_scratch_);
        send(pi, cb::SystemChatComponent{{text_scratch_.data, text_scratch_.len}});
        return;
    }
    ChatMessage m;
    m.sender = players_[pi].uuid;
    m.content = c.message;  // (a copy: the packet is a view into the inflate buffer)
    broadcast_chat(m, reg::chat_type::kChat, display_name(pi));
    detect_spam(pi, true);
}

// ---------------------------------------------------------------------------- what goes out

// PlayerChatMessage.decoratedContent
Text Game::chat_content(const ChatMessage& m) { return m.decorated ? m.shown : Text::literal(m.content); }

// ChatType.Bound.decorate: the message as a client shows it - "<Steve> hello", "[Steve] hello",
// "Steve whispers to you: hello". Clients do this themselves from the chat type; the server
// does it for its log and for the console.
Text Game::decorate_chat(u32 chat_type, const Text& name, const Text* target, const Text& content) {
    namespace ct = proto::gen::chat_type;
    const ct::Decoration& d = ct::kChat[chat_type];
    Text out = Text::translatable(d.translation_key);
    for (u32 i = 0; i < d.count; ++i) {
        switch (d.params[i]) {
            case ct::kSender: out.arg(name); break;
            case ct::kTarget: out.arg(target ? *target : Text()); break;
            case ct::kContent: out.arg(content); break;
        }
    }
    return out;
}

// ComponentUtils.formatList(entities, Entity::getDisplayName): the one name, or all of them
// with grey commas between.
Text Game::join_names(const std::vector<u32>& slots) const {
    if (slots.size() == 1) return display_name(ents_.by_slot(slots[0]));
    Text names;
    for (usize i = 0; i < slots.size(); ++i) {
        if (i) names.append(Text::literal(", ").color(Color::kGray));
        names.append(display_name(ents_.by_slot(slots[i])));
    }
    return names;
}

// One message, as bytes ready to be handed to any number of players (deliver_chat): the tail of
// a player_chat in chat_tail_, or - for a message that is nobody's - a whole disguised_chat
// frame in scratch_.
void Game::encode_chat(const ChatMessage& m, u32 chat_type, const Text& name, const Text* target) {
    chat_name_.clear();
    name.encode(chat_name_);
    chat_target_.clear();
    if (target) target->encode(chat_target_);
    const cb::ChatTypeBound bound{chat_type, {chat_name_.data, chat_name_.len}, {chat_target_.data, chat_target_.len}};
    chat_shown_.clear();
    if (m.system()) {  // OutgoingChatMessage.Disguised: the content as it is shown, and no sender
        chat_content(m).encode(chat_shown_);
        scratch_.clear();
        write_packet(scratch_, s_.compression_threshold, cb::DisguisedChat{{chat_shown_.data, chat_shown_.len}, bound});
        return;
    }
    if (m.decorated) m.shown.encode(chat_shown_);
    const cb::PlayerChatTail tail{m.sender, m.content, now_ms(), {chat_shown_.data, chat_shown_.len}, bound};
    chat_tail_.clear();
    chat_tail_.commit(tail.encode(chat_tail_.reserve(tail.bound())));
}

// ServerPlayer.sendChatMessage: only to who shows the chat in full. sendPlayerChatMessage counts
// what it sends (nextChatIndex++, the packet's first field: the first is 0), and the client
// counts along.
void Game::deliver_chat(u32 pi, bool system) {
    if (chat_visibility_[pi] != kFull) return;
    if (system)
        out(pi).append(scratch_.data, scratch_.len);
    else
        frame_stored(out(pi), s_.compression_threshold, cb::PlayerChatTail::kId, chat_index_[pi]++,
                     {chat_tail_.data, chat_tail_.len});
}

// PlayerList.broadcastChatMessage: into the log - "[Not Secure]", as vanilla tags whatever is not
// signed - and to everyone, the sender included.
void Game::broadcast_chat(const ChatMessage& m, u32 chat_type, const Text& name) {
    LOG_INFO("[Not Secure] %s", decorate_chat(chat_type, name, nullptr, chat_content(m)).plain().c_str());
    encode_chat(m, chat_type, name, nullptr);
    const bool system = m.system();
    const u32 n = static_cast<u32>(players_.size());
    for (u32 pi = 0; pi < n; ++pi) deliver_chat(pi, system);
}

// To one player (a whisper, and its echo to who whispered).
void Game::send_chat(u32 pi, const ChatMessage& m, u32 chat_type, const Text& name, const Text* target) {
    encode_chat(m, chat_type, name, target);
    deliver_chat(pi, m.system());
}

// PlayerList.broadcastSystemMessage: the console (MinecraftServer.sendSystemMessage), and every
// player who takes system messages.
void Game::broadcast_system(const Text& message, u32 except) {
    LOG_INFO("%s", message.plain().c_str());
    text_scratch_.clear();
    message.encode(text_scratch_);
    scratch_.clear();
    write_packet(scratch_, s_.compression_threshold, cb::SystemChatComponent{{text_scratch_.data, text_scratch_.len}});
    const u32 n = static_cast<u32>(players_.size());
    for (u32 pi = 0; pi < n; ++pi)
        if (pi != except && chat_visibility_[pi] != kHidden) out(pi).append(scratch_.data, scratch_.len);
}

}  // namespace server
