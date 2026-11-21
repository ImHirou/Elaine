// SPDX-License-Identifier: GPL-3.0-or-later
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "proto/packets.hpp"
#include "proto/wire.hpp"

using namespace proto;

TEST(Reader, PrimitivesRoundTrip) {
    u8 buf[128];
    u8* p = buf;
    p = put_u8(p, 0xAB);
    p = put_be16(p, 0x1234);
    p = put_be32(p, 0xDEADBEEF);
    p = put_be64(p, 0x0102030405060708ull);
    p = put_f32(p, 1.5f);
    p = put_f64(p, -2.25);
    p = put_varint(p, 300);
    p = put_str(p, "hello");
    p = put_uuid(p, Uuid{1, 2});
    Reader r(buf, static_cast<usize>(p - buf));
    EXPECT_EQ(r.u8v(), 0xAB);
    EXPECT_EQ(r.be16(), 0x1234);
    EXPECT_EQ(r.be32(), 0xDEADBEEFu);
    EXPECT_EQ(r.be64(), 0x0102030405060708ull);
    EXPECT_EQ(r.f32(), 1.5f);
    EXPECT_EQ(r.f64(), -2.25);
    EXPECT_EQ(r.varint(), 300u);
    EXPECT_EQ(r.str(16), "hello");
    EXPECT_EQ(r.uuid(), (Uuid{1, 2}));
    EXPECT_TRUE(r.done());
}

TEST(Reader, UnderflowIsSticky) {
    const u8 buf[3] = {1, 2, 3};
    Reader r(buf, 3);
    EXPECT_EQ(r.be32(), 0u);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.u8v(), 0);  // nothing is readable after an error
    EXPECT_FALSE(r.done());
}

TEST(Reader, StringLengthLimit) {
    u8 buf[64];
    u8* p = put_str(buf, "0123456789");
    Reader ok(buf, static_cast<usize>(p - buf));
    EXPECT_EQ(ok.str(10), "0123456789");
    Reader bad(buf, static_cast<usize>(p - buf));
    EXPECT_TRUE(bad.str(9).empty());
    EXPECT_FALSE(bad.ok);
}

TEST(Wire, BlockPosPacking) {
    // Values from the protocol documentation's worked example.
    EXPECT_EQ(pack_block_pos(18357644, 831, -20882616), 0x4607632C15B4833Full);
    EXPECT_EQ(pack_block_pos(0, 0, 0), 0u);
}

TEST(Wire, BitSetIsLittleEndianBytes) {
    // What BitSet.toByteArray() returns for these sets, behind a VarInt byte count.
    const auto enc = [](u64 bits) {
        u8 buf[kBitSetMaxBytes + 8];
        std::memset(buf, 0xEE, sizeof buf);
        u8* end = put_bit_set(buf, bits);
        EXPECT_LE(end - buf, static_cast<std::ptrdiff_t>(kBitSetMaxBytes));
        for (usize i = kBitSetMaxBytes; i < sizeof buf; ++i) EXPECT_EQ(buf[i], 0xEE) << "wrote past the bound";
        return std::vector<u8>(buf, end);
    };
    EXPECT_EQ(enc(0), (std::vector<u8>{0x00}));
    EXPECT_EQ(enc(0b110), (std::vector<u8>{0x01, 0x06}));
    EXPECT_EQ(enc(1u << 11), (std::vector<u8>{0x02, 0x00, 0x08}));
    EXPECT_EQ(enc(0x3FFFFFF), (std::vector<u8>{0x04, 0xFF, 0xFF, 0xFF, 0x03}));  // all 26 light sections
    EXPECT_EQ(enc(1ull << 63), (std::vector<u8>{0x08, 0, 0, 0, 0, 0, 0, 0, 0x80}));
}

TEST(Wire, NbtTextIsNamelessStringTag) {
    u8 buf[64];
    const u8* end = put_nbt_text(buf, "hi");
    ASSERT_EQ(end - buf, 5);
    const u8 expect[] = {8, 0, 2, 'h', 'i'};
    EXPECT_EQ(0, std::memcmp(buf, expect, 5));
}

TEST(Wire, ModifiedUtf8) {
    u8 buf[64];
    // U+1F600 as UTF-8 (F0 9F 98 80) becomes the surrogate pair D83D DE00, 3 bytes each.
    const u8* end = put_mutf8(buf, "\xF0\x9F\x98\x80");
    const u8 expect[] = {0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80};
    ASSERT_EQ(end - buf, 6);
    EXPECT_EQ(0, std::memcmp(buf, expect, 6));
    end = put_mutf8(buf, std::string_view("a\0b", 3));
    const u8 expect2[] = {'a', 0xC0, 0x80, 'b'};
    ASSERT_EQ(end - buf, 4);
    EXPECT_EQ(0, std::memcmp(buf, expect2, 4));
    end = put_mutf8(buf, "\xD0\x9F\xE2\x82\xAC");  // 2- and 3-byte sequences pass through
    ASSERT_EQ(end - buf, 5);
}

TEST(Wire, JsonEscape) {
    std::string out;
    json_escape(out, "a\"b\\c\n\x01");
    EXPECT_EQ(out, "a\\\"b\\\\c\\n\\u0001");
}

TEST(Packets, LoginPlayLayout) {
    // Field-by-field check of the 26.3 layout (see ClientboundLoginPacket / CommonPlayerSpawnInfo).
    const std::string_view dims[] = {"minecraft:overworld"};
    cb::Login l{};
    l.entity_id = 7;
    l.dimensions = dims;
    l.max_players = 20;
    l.view_distance = 10;
    l.simulation_distance = 8;
    l.show_death_screen = true;
    l.dimension_type = 3;
    l.dimension = "minecraft:overworld";
    l.hashed_seed = 0x1122334455667788ull;
    l.game_mode = 1;
    l.previous_game_mode = -1;
    l.sea_level = 63;
    l.online_mode = true;
    u8 buf[256];
    const u8* end = l.encode(buf);
    ASSERT_LE(static_cast<usize>(end - buf), l.bound());
    Reader r(buf, static_cast<usize>(end - buf));
    EXPECT_EQ(r.be32(), 7u);
    EXPECT_FALSE(r.boolean());
    EXPECT_EQ(r.varint(), 1u);
    EXPECT_EQ(r.str(64), "minecraft:overworld");
    EXPECT_EQ(r.varint(), 20u);
    EXPECT_EQ(r.varint(), 10u);
    EXPECT_EQ(r.varint(), 8u);
    EXPECT_FALSE(r.boolean());
    EXPECT_TRUE(r.boolean());
    EXPECT_FALSE(r.boolean());
    EXPECT_EQ(r.varint(), 3u);
    EXPECT_EQ(r.str(64), "minecraft:overworld");
    EXPECT_EQ(r.be64(), 0x1122334455667788ull);
    EXPECT_EQ(r.varint(), 1u);  // game mode as VarInt
    EXPECT_EQ(r.varint(), 0u);  // no previous game mode -> optional VarInt 0
    EXPECT_FALSE(r.boolean());
    EXPECT_FALSE(r.boolean());
    EXPECT_FALSE(r.boolean());  // no death location
    EXPECT_EQ(r.varint(), 0u);
    EXPECT_EQ(r.varint(), 63u);
    EXPECT_TRUE(r.boolean());   // online mode
    EXPECT_FALSE(r.boolean());  // enforces secure chat
    EXPECT_TRUE(r.done());
}

TEST(Packets, PlayerInfoAddLayout) {
    const ProfileProperty props[] = {{"textures", "dGV4", "c2ln"}, {"plain", "v", ""}};
    u8 wire[256];
    const u8* wend = put_profile_wire(wire, Uuid{1, 2}, "Steve", props);
    ASSERT_LE(static_cast<usize>(wend - wire), profile_wire_bound("Steve", props));
    const cb::PlayerInfoEntry entries[] = {{{wire, static_cast<usize>(wend - wire)}, 1, true, 42},
                                           {{wire, static_cast<usize>(wend - wire)}, 0, false, 0}};
    const cb::PlayerInfoAdd pkt{entries};
    u8 buf[512];
    const u8* end = pkt.encode(buf);
    ASSERT_LE(static_cast<usize>(end - buf), pkt.bound());
    Reader r(buf, static_cast<usize>(end - buf));
    EXPECT_EQ(r.u8v(), 0b00011101);  // ADD_PLAYER | UPDATE_GAME_MODE | UPDATE_LISTED | UPDATE_LATENCY
    EXPECT_EQ(r.varint(), 2u);
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(r.uuid(), (Uuid{1, 2}));
        EXPECT_EQ(r.str(16), "Steve");
        EXPECT_EQ(r.varint(), 2u);
        EXPECT_EQ(r.str(64), "textures");
        EXPECT_EQ(r.str(64), "dGV4");
        EXPECT_TRUE(r.boolean());
        EXPECT_EQ(r.str(64), "c2ln");
        EXPECT_EQ(r.str(64), "plain");
        EXPECT_EQ(r.str(64), "v");
        EXPECT_FALSE(r.boolean());  // unsigned property: no signature string follows
        EXPECT_EQ(r.varint(), i == 0 ? 1u : 0u);
        EXPECT_EQ(r.boolean(), i == 0);
        EXPECT_EQ(r.varint(), i == 0 ? 42u : 0u);
    }
    EXPECT_TRUE(r.done());
}

// ---------------------------------------------------------------------------- entities

namespace {
std::string hex(const u8* b, const u8* e) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (; b != e; ++b) {
        out += digits[*b >> 4];
        out += digits[*b & 15];
    }
    return out;
}
template <class P>
std::string body_hex(const P& pkt) {
    u8 buf[256];
    std::memset(buf, 0xEE, sizeof buf);
    const u8* end = pkt.encode(buf);
    EXPECT_LE(static_cast<usize>(end - buf), pkt.bound());
    return hex(buf, end);
}

// LpVec3.read
void read_lp_vec3(Reader& r, double out[3]) {
    const u64 lowest = r.u8v();
    out[0] = out[1] = out[2] = 0;
    if (lowest == 0) return;
    const u64 middle = r.u8v();
    const u64 buffer = (static_cast<u64>(r.be32()) << 16) | (middle << 8) | lowest;
    u64 scale = lowest & 3;
    if (lowest & 4) scale |= static_cast<u64>(r.varint()) << 2;
    const auto unpack = [](u64 v) { return std::min(static_cast<double>(v & 32767), 32766.0) * 2.0 / 32766.0 - 1.0; };
    out[0] = unpack(buffer >> 3) * static_cast<double>(scale);
    out[1] = unpack(buffer >> 18) * static_cast<double>(scale);
    out[2] = unpack(buffer >> 33) * static_cast<double>(scale);
}
}  // namespace

TEST(Packets, EntityLayoutsAreWhatVanillaSent) {
    // Every string is a packet body (after the packet id) that a vanilla 26.3 server sent to a
    // test client watching another one - entity id 14 - join, walk, turn, sneak, jump and leave.
    namespace ed = entity_data;
    const Uuid bob{0x21b1e65c1df43fbfull, 0x94e2dfb487aa523cull};
    ASSERT_EQ(entity_type::kPlayer, 159u);

    // The pairing bundle of a player that just joined, and of one that has since jumped (the
    // 0.42 is Player.jumpFromGround) and turned.
    EXPECT_EQ(body_hex(cb::AddEntity{14, bob, entity_type::kPlayer, 8.5, -60.0, 1.5, 0, 0, 0, 0, 0, 0, 0}),
              "0e21b1e65c1df43fbf94e2dfb487aa523c9f014021000000000000c04e0000000000003ff80000000000000000000000");
    EXPECT_EQ(body_hex(cb::AddEntity{14, bob, entity_type::kPlayer, 34.5, -60.0, 3.0, 0, 0.42f, 0, 0xF1, 0x75, 0x75, 0}),
              "0e21b1e65c1df43fbf94e2dfb487aa523c9f014041400000000000c04e0000000000004008000000000000f9ff7fff6b81f1757500");
    u8 e[64];
    const u8* q = cb::put_entity_byte(cb::put_entity_float(e, ed::living_entity::kHealth, 20.0f),
                                      ed::avatar::kPlayerModeCustomisation, 0x7F);
    EXPECT_EQ(body_hex(cb::SetEntityData{14, {e, static_cast<usize>(q - e)}}), "0e090341a0000010007fff");

    // Sneak: the flag, then the pose a tick later. Options: left-handed, cape off.
    q = cb::put_entity_byte(e, ed::entity::kSharedFlags, 0x02);
    EXPECT_EQ(body_hex(cb::SetEntityData{14, {e, static_cast<usize>(q - e)}}), "0e000002ff");
    q = cb::put_entity_varint(e, ed::entity::kPose, ed::serializer::kPose, pose::kCrouching);
    EXPECT_EQ(body_hex(cb::SetEntityData{14, {e, static_cast<usize>(q - e)}}), "0e061405ff");
    q = cb::put_entity_byte(cb::put_entity_varint(e, ed::avatar::kPlayerMainHand, ed::serializer::kHumanoidArm, 0),
                            ed::avatar::kPlayerModeCustomisation, 0x7E);
    EXPECT_EQ(body_hex(cb::SetEntityData{14, {e, static_cast<usize>(q - e)}}), "0e0f2a0010007eff");

    // Movement. A quarter block along x in a step of 2 ticks, on the ground; the first update of
    // an entity's life (a plain zero delta, in the air); half a block up; the largest step back.
    EXPECT_EQ(body_hex(cb::MoveEntityPos{14, true, 2, 0x0400, 0, 0}), "0e0302040000000000");
    EXPECT_EQ(body_hex(cb::MoveEntityPos{14, false, 0, 0, 0, 0}), "0e00000000000000");
    EXPECT_EQ(body_hex(cb::MoveEntityPos{14, false, 2, 0, 0x0800, 0}), "0e0202000008000000");
    EXPECT_EQ(body_hex(cb::MoveEntityPos{14, true, 2, -32768, 0, 0}), "0e0302800000000000");
    EXPECT_EQ(body_hex(cb::MoveEntityPosRot{14, true, 2, 0, 0, 0x0400, 0x40, 0xF1}), "0e030200000000040040f1");
    EXPECT_EQ(body_hex(cb::MoveEntityRot{14, true, 7, 3}), "0e010703");
    EXPECT_EQ(body_hex(cb::RotateHead{14, 0x0E}), "0e0e");
    EXPECT_EQ(body_hex(cb::EntityPositionSync{14, 2, 8.5, -60.0, 1.5, 0.0f, 0.0f, true}),
              "0e01014021000000000000c04e0000000000003ff800000000000002000000000000000001");
    EXPECT_EQ(body_hex(cb::EntityPositionSync{14, 2, 10.5, -59.5, 3.0, 165.0f, -20.0f, false}),
              "0e01014025000000000000c04dc0000000000040080000000000000243250000c1a0000000");
    EXPECT_EQ(body_hex(cb::SetEntityMotion{14, 0, 0.42f, 0}), "0ef9ff7fff6b81");
    EXPECT_EQ(body_hex(cb::RemoveEntity{14}), "010e");
    EXPECT_EQ(body_hex(cb::BundleDelimiter{}), "");

    // Not in the capture (players are living entities, their paths always have steps): the plain
    // PositionPath of everything else, type 0 and one Vec3, as PositionPath.Linear.STREAM_CODEC writes it.
    EXPECT_EQ(body_hex(cb::EntityPositionSync{300, 0, 8.5, -60.0, 1.5, 90.0f, 0.0f, false}),
              "ac02004021000000000000c04e0000000000003ff800000000000042b400000000000000");
}

TEST(EntityCodec, AnglesAndDeltasAreJavas) {
    // Mth.packDegrees: the pairs the vanilla capture shows for these angles.
    EXPECT_EQ(pack_degrees(0.0f), 0);
    EXPECT_EQ(pack_degrees(10.0f), 7);
    EXPECT_EQ(pack_degrees(5.0f), 3);
    EXPECT_EQ(pack_degrees(20.0f), 0x0E);
    EXPECT_EQ(pack_degrees(90.0f), 0x40);
    EXPECT_EQ(pack_degrees(165.0f), 0x75);
    EXPECT_EQ(pack_degrees(-20.0f), 0xF1);  // floor, not truncation: -14.2 -> -15
    EXPECT_EQ(pack_degrees(-90.0f), 0xC0);
    EXPECT_EQ(pack_degrees(360.0f), 0);
    EXPECT_EQ(pack_degrees(-0.5f), 0xFF);
    for (int b = -128; b < 128; ++b) EXPECT_EQ(pack_degrees(unpack_degrees(static_cast<u8>(b))), static_cast<u8>(b)) << b;
    EXPECT_EQ(pack_degrees(std::numeric_limits<float>::quiet_NaN()), 0);  // (int) NaN is 0 in Java; no trap here either
    EXPECT_EQ(pack_degrees(1.0e30f), 0xFF);  // saturates to Integer.MAX_VALUE, whose low byte is 0xFF

    EXPECT_EQ(wrap_degrees(270.0f), -90.0f);
    EXPECT_EQ(wrap_degrees(-180.0f), -180.0f);
    EXPECT_EQ(wrap_degrees(180.0f), -180.0f);
    EXPECT_EQ(wrap_degrees(725.0f), 5.0f);
    EXPECT_EQ(wrap_degrees(-190.0f), 170.0f);

    // VecDeltaCodec.encode is Math.round(v * 4096): nearest, ties up - also below zero, and for
    // the largest double below one half, where floor(x + 0.5) would say 1.
    EXPECT_EQ(delta_units(0.25), 1024);
    EXPECT_EQ(delta_units(-60.0), -245760);
    EXPECT_EQ(delta_units(0.5 / 4096), 1);
    EXPECT_EQ(delta_units(-0.5 / 4096), 0);
    EXPECT_EQ(delta_units(1.5 / 4096), 2);
    EXPECT_EQ(delta_units(-1.5 / 4096), -1);
    EXPECT_EQ(delta_units(0.49999999999999994 / 4096), 0);
    EXPECT_EQ(delta_units(3.0e7), 122880000000ll);
    EXPECT_TRUE(delta_fits(32767, -32768, 0));
    EXPECT_FALSE(delta_fits(32768, 0, 0));
    EXPECT_FALSE(delta_fits(0, -32769, 0));
    EXPECT_FALSE(delta_fits(0, 0, 1ll << 40));
}

TEST(EntityCodec, LpVec3) {
    const auto enc = [](double x, double y, double z) {
        u8 buf[32];
        std::memset(buf, 0xEE, sizeof buf);
        const u8* end = put_lp_vec3(buf, x, y, z);
        EXPECT_LE(end - buf, static_cast<std::ptrdiff_t>(kLpVec3MaxBytes));
        return std::vector<u8>(static_cast<const u8*>(buf), end);
    };
    EXPECT_EQ(enc(0, 0, 0), (std::vector<u8>{0}));
    EXPECT_EQ(enc(1e-5, -2e-5, 3e-5), (std::vector<u8>{0}));  // below LpVec3.ABS_MIN_VALUE on every axis
    EXPECT_EQ(enc(std::numeric_limits<double>::quiet_NaN(), 0, 0), (std::vector<u8>{0}));
    const std::vector<u8> jump = enc(0, 0.42f, 0);
    EXPECT_EQ(hex(jump.data(), jump.data() + jump.size()), "f9ff7fff6b81");  // from the vanilla capture

    // Whatever goes in comes out of vanilla's reader within one 15-bit step of the scale.
    std::mt19937 rng(17);
    for (int i = 0; i < 20000; ++i) {
        const double mag = std::pow(10.0, static_cast<double>(rng() % 90) / 10.0 - 4.0);  // 1e-4 .. 1e5
        const double v[3] = {(static_cast<double>(rng() % 2001) - 1000.0) / 1000.0 * mag,
                             (static_cast<double>(rng() % 2001) - 1000.0) / 1000.0 * mag,
                             (static_cast<double>(rng() % 2001) - 1000.0) / 1000.0 * mag};
        const std::vector<u8> bytes = enc(v[0], v[1], v[2]);
        Reader r(bytes.data(), bytes.size());
        double out[3];
        read_lp_vec3(r, out);
        ASSERT_TRUE(r.done()) << i;
        const double longest = std::max({std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2])});
        const double step = longest < 3.051944088384301E-5 ? 3.1e-5 : std::ceil(longest) / 32766.0 * 1.001;
        for (int a = 0; a < 3; ++a) ASSERT_NEAR(out[a], v[a], step) << i;
        ASSERT_EQ(bytes.size() > 6, std::ceil(longest) > 3)
            << "the scale spills into a VarInt exactly when it needs more than two bits";
    }
    // The clamp: nothing larger than LpVec3.ABS_MAX_VALUE leaves, whatever comes in.
    const std::vector<u8> huge = enc(1e300, -1e300, 0);
    Reader r(huge.data(), huge.size());
    double out[3];
    read_lp_vec3(r, out);
    EXPECT_TRUE(r.done());
    EXPECT_NEAR(out[0], 1.7179869183E10, 1.0e6);
    EXPECT_NEAR(out[1], -1.7179869183E10, 1.0e6);
}

TEST(Packets, TeleportEntityLayout) {
    // ClientboundTeleportEntityPacket.STREAM_CODEC: VAR_INT id, PositionMoveRotation.STREAM_CODEC
    // (Vec3 position, Vec3 deltaMovement, FLOAT yRot, FLOAT xRot), Relative.SET_STREAM_CODEC (an
    // INT of bits), BOOL onGround.
    const cb::TeleportEntity t{300, 8.5, -57.0, -1000.25, 0.125, -0.0784, 0.0, 90.0f, -45.0f, 0, true};
    u8 buf[96];
    const u8* end = t.encode(buf);
    ASSERT_LE(static_cast<usize>(end - buf), t.bound());
    Reader r(buf, static_cast<usize>(end - buf));
    EXPECT_EQ(r.varint(), 300u);
    EXPECT_EQ(r.f64(), 8.5);
    EXPECT_EQ(r.f64(), -57.0);
    EXPECT_EQ(r.f64(), -1000.25);
    EXPECT_EQ(r.f64(), 0.125);
    EXPECT_EQ(r.f64(), -0.0784);
    EXPECT_EQ(r.f64(), 0.0);
    EXPECT_EQ(r.f32(), 90.0f);
    EXPECT_EQ(r.f32(), -45.0f);
    EXPECT_EQ(r.be32(), 0u);  // no Relative: everything is absolute
    EXPECT_TRUE(r.boolean());
    EXPECT_TRUE(r.done());
}

TEST(Packets, PlayerCommandAndMoveStatus) {
    u8 buf[16];
    const u8* end = put_varint(put_varint(put_varint(buf, 300), sb::PlayerCommand::kStartSprinting), 0);
    Reader r(buf, static_cast<usize>(end - buf));
    const auto c = sb::PlayerCommand::decode(r);
    EXPECT_TRUE(r.done());
    EXPECT_EQ(c.entity_id, 300u);
    EXPECT_EQ(c.action, 1u);  // START_SPRINTING: what the vanilla client's sprint key sent
    end = put_varint(put_varint(put_varint(buf, 1), sb::PlayerCommand::kActionCount), 0);
    Reader bad(buf, static_cast<usize>(end - buf));
    sb::PlayerCommand::decode(bad);
    EXPECT_FALSE(bad.ok);

    const u8 flags[] = {3};
    Reader s(flags, 1);
    const auto m = sb::MovePlayer::decode_status(s);
    EXPECT_TRUE(s.done());
    EXPECT_TRUE(m.flags & sb::MovePlayer::kOnGround);
    EXPECT_TRUE(m.flags & sb::MovePlayer::kHorizontalCollision);
}

TEST(Packets, InventoryLayoutsAreWhatVanillaSent) {
    // Every string is a packet body (after the packet id) that a vanilla 26.3 server sent while a
    // test client - entity 15 - filled its creative inventory, wore things, swapped hands,
    // dropped items, picked them up and clicked through its menu, with entity 13 watching
    // (tools/vanilla/inventory_probe.py). Item ids: stone 1, dirt 55, elytra 974, diamond helmet
    // 1084, shield 1446.
    const ItemView none{}, stone64{64, 1, {}}, helmet{1, 1084, {}}, shield{1, 1446, {}};

    // What an entity holds and wears. One slot; an emptied hand; three slots emptied in one tick.
    cb::EquipmentEntry one[] = {{0, stone64}};
    EXPECT_EQ(body_hex(cb::SetEquipment{15, one}), "0f0040010000");
    one[0] = {5, helmet};
    EXPECT_EQ(body_hex(cb::SetEquipment{15, one}), "0f0501bc080000");
    one[0] = {1, shield};
    EXPECT_EQ(body_hex(cb::SetEquipment{15, one}), "0f0101a60b0000");
    one[0] = {0, none};
    EXPECT_EQ(body_hex(cb::SetEquipment{15, one}), "0f0000");
    const cb::EquipmentEntry three[] = {{0, none}, {1, none}, {4, none}};
    EXPECT_EQ(body_hex(cb::SetEquipment{15, three}), "0f800081000400");
    EXPECT_EQ(body_hex(cb::EntityEvent{15, cb::EntityEvent::kSwapHands}), "0000000f37");
    // The arm swing of entity 15 dropping an item: main hand, SwingAnimation.DEFAULT (a whack of
    // 6 ticks). Then entity 14 (tools/vanilla/swing_probe.py) placing a block with the off hand,
    // and punching with a wooden spear in hand: a stab of 13 ticks.
    EXPECT_EQ(body_hex(cb::SwingAnimation{15, cb::SwingAnimation::kMainHand, {}}), "0f000106");
    EXPECT_EQ(body_hex(cb::SwingAnimation{14, cb::SwingAnimation::kOffHand, {}}), "0e010106");
    EXPECT_EQ(body_hex(cb::SwingAnimation{14, cb::SwingAnimation::kMainHand, {swing_animation::kStab, 13}}), "0e00020d");
    // The helmet going on, heard at (6.5, -60, -9.5): sound 68 is item.armor.equip_diamond.
    EXPECT_EQ(body_hex(cb::Sound{68, cb::Sound::kSourcePlayers, 6.5, -60.0, -9.5, 1.0f, 1.0f, 0x8442a8901f6def28ull}),
              "450700000034fffffe20ffffffb43f8000003f8000008442a8901f6def28");

    // An item on the ground: its stack as entity data (accessor 8, serializer 7), and being taken.
    u8 e[64];
    const u8* q = cb::put_entity_item(e, entity_data::item_entity::kItem, ItemView{1, 1, {}});
    EXPECT_EQ(body_hex(cb::SetEntityData{16, {e, static_cast<usize>(q - e)}}), "10080701010000ff");
    EXPECT_EQ(body_hex(cb::TakeItemEntity{19, 15, 64}), "130f40");
    // Ours alone, by the same rules: an entity that does not fall says so (accessor 5, a boolean).
    q = cb::put_entity_item(cb::put_entity_bool(e, entity_data::entity::kNoGravity, true), entity_data::item_entity::kItem,
                            stone64);
    EXPECT_EQ(body_hex(cb::SetEntityData{16, {e, static_cast<usize>(q - e)}}), "10050801080740010000ff");

    // The menu. Slot packets carry the container id (0), a state id, the slot and the stack.
    EXPECT_EQ(body_hex(cb::ContainerSetSlot{0, 2, 36, shield}), "0002002401a60b0000");
    EXPECT_EQ(body_hex(cb::ContainerSetSlot{0, 3, 45, stone64}), "0003002d40010000");
    EXPECT_EQ(body_hex(cb::ContainerSetSlot{0, 8, 9, none}), "0008000900");
    EXPECT_EQ(body_hex(cb::SetCursorItem{ItemView{10, 1, {}}}), "0a010000");
    EXPECT_EQ(body_hex(cb::SetCursorItem{ItemView{5, 55, {}}}), "05370000");
    EXPECT_EQ(body_hex(cb::SetPlayerInventory{10, ItemView{20, 1, {}}}), "0a14010000");
    EXPECT_EQ(body_hex(cb::SetPlayerInventory{11, ItemView{7, 55, {}}}), "0b07370000");
    // All 46 slots, then the cursor: state 14; 5 stone in slot 10, 7 in 12, a chestplate in 13,
    // 4 pumpkins in 14, 5 stone in 20, 63 stone on the cursor.
    ItemView slots[46];
    slots[10] = {5, 1, {}};
    slots[12] = {7, 1, {}};
    slots[13] = {1, 1081, {}};
    slots[14] = {4, 431, {}};
    slots[20] = {5, 1, {}};
    EXPECT_EQ(
        body_hex(cb::ContainerSetContent{0, 14, slots, ItemView{63, 1, {}}}),
        "000e2e0000000000000000000005010000000701000001b908000004af0300000000000000050100000000000000000000000000000000000000"
        "00000000000000003f010000");
    EXPECT_EQ(body_hex(cb::SetHeldSlot{3}), "03");

    // A stack with components keeps them as they stand in its patch.
    const u8 patch[] = {0x01, 0x00, 0x03, 0xAC, 0x02};  // one added (damage = 300), none removed
    EXPECT_EQ(body_hex(cb::SetCursorItem{ItemView{1, 1084, patch}}), "01bc08010003ac02");
    EXPECT_EQ(item_bound(ItemView{1, 1084, patch}), 8u);
    EXPECT_EQ(item_bound(none), 1u);
}

TEST(Packets, ContainerAndExplosionLayoutsAreWhatVanillaSent) {
    // Packet bodies a vanilla 26.3 server sent while a test client opened a chest, a double chest,
    // a crafting table and a furnace, and lit a TNT four blocks away (tools/vanilla/container_probe.py).
    // A title is a text component: the tag {translate: "container.chest"}.
    const auto title = [](std::string_view key) {
        std::vector<u8> t = {
            0x0a, 0x08, 0x00, 0x09, 't', 'r', 'a', 'n', 's', 'l', 'a', 't', 'e', 0x00, static_cast<u8>(key.size())};
        t.insert(t.end(), key.begin(), key.end());
        t.push_back(0x00);
        return t;
    };
    const std::vector<u8> chest = title("container.chest"), furnace = title("container.furnace");
    EXPECT_EQ(body_hex(cb::OpenScreen{1, 2, chest}),
              "01020a0800097472616e736c617465000f636f6e7461696e65722e636865737400");  // generic_9x3
    EXPECT_EQ(body_hex(cb::OpenScreen{4, 14, furnace}), "040e0a0800097472616e736c6174650011636f6e7461696e65722e6675726e61636500");
    EXPECT_EQ(body_hex(cb::ContainerSetData{4, 3, 200}), "04000300c8");  // the furnace's cooking total
    EXPECT_EQ(body_hex(cb::ContainerSetData{4, 0, 0}), "0400000000");
    EXPECT_EQ(body_hex(cb::ContainerClose{2}), "02");
    // A chest's lid at (4, -60, -13): event 1, one opener; block 245 is minecraft:chest.
    EXPECT_EQ(body_hex(cb::BlockEvent{4, -60, -13, 1, 1, 245}), "0000013fffff3fc40101f501");

    // The explosion, field by field - and written back from what was read.
    const std::string sent =
        "401143ebc8a5453fc04df828f5c00000c0252bbdcdc4144240800000000002ac013f96bf3cfd52e2e93fc82178bbc7ace8bfde4ce5e922e97e1d"
        "c00502453f0000003f80000001483f8000003f8000000101";
    std::vector<u8> raw;
    for (usize i = 0; i < sent.size(); i += 2) raw.push_back(static_cast<u8>(std::stoi(sent.substr(i, 2), nullptr, 16)));
    Reader r(raw.data(), raw.size());
    cb::Explode e{};
    e.x = r.f64(), e.y = r.f64(), e.z = r.f64();
    EXPECT_DOUBLE_EQ(e.y, -60.0 + static_cast<double>(0.98f) * 0.0625);  // a sixteenth of a TNT's height above the ground
    e.radius = r.f32();
    EXPECT_EQ(e.radius, 4.0f);
    e.block_count = static_cast<i32>(r.be32());
    EXPECT_EQ(e.block_count, 684);
    e.knocked = r.boolean();
    ASSERT_TRUE(e.knocked);  // the client stood four blocks off
    e.kx = r.f64(), e.ky = r.f64(), e.kz = r.f64();
    EXPECT_NEAR(std::sqrt(e.kx * e.kx + e.ky * e.ky + e.kz * e.kz), 0.51, 0.01);
    e.particle = r.varint();
    EXPECT_EQ(e.particle, 29u);  // explosion_emitter
    e.sound = r.varint() - 1;    // a holder
    EXPECT_EQ(e.sound, 703u);    // entity.generic.explode
    cb::ExplosionParticle particles[2];
    ASSERT_EQ(r.varint(), 2u);  // Level.DEFAULT_EXPLOSION_BLOCK_PARTICLES
    for (cb::ExplosionParticle& b : particles) {
        b.particle = r.varint();
        b.scaling = r.f32();
        b.speed = r.f32();
        b.weight = r.varint();
    }
    EXPECT_EQ(particles[0].particle, 69u);  // poof, half size
    EXPECT_EQ(particles[0].scaling, 0.5f);
    EXPECT_EQ(particles[1].particle, 72u);  // smoke
    EXPECT_EQ(particles[1].weight, 1u);
    e.block_particles = particles;
    e.play_sound = r.boolean();
    EXPECT_TRUE(e.play_sound);
    EXPECT_TRUE(r.done());
    EXPECT_EQ(body_hex(e), sent);
}

// ServerboundChunkBatchReceivedPacket: one float, how many chunks a tick the client would like.
TEST(Packets, BlockUpdateLayoutsAreTheJars) {
    // ClientboundLevelEventPacket.write: writeInt(type), writeBlockPos, writeInt(data), writeBoolean(global).
    // A torch (state 4926) breaking at (4, -60, -13): event 2001.
    const std::string event = body_hex(cb::LevelEvent{2001, 4, -60, -13, 4926, false});
    EXPECT_EQ(event,
              "000007d1"
              "0000013fffff3fc4"
              "0000133e"
              "00");
    std::vector<u8> raw;
    for (usize i = 0; i < event.size(); i += 2) raw.push_back(static_cast<u8>(std::stoi(event.substr(i, 2), nullptr, 16)));
    Reader r(raw.data(), raw.size());
    EXPECT_EQ(r.be32(), 2001u);
    const BlockPos pos = unpack_block_pos(r.be64());
    EXPECT_EQ(pos.x, 4);
    EXPECT_EQ(pos.y, -60);
    EXPECT_EQ(pos.z, -13);
    EXPECT_EQ(r.be32(), 4926u);
    EXPECT_FALSE(r.boolean());
    EXPECT_TRUE(r.done());
    // ClientboundAddTransientBlockPacket: BlockPos, then the state as a VarInt - block_update's layout.
    EXPECT_EQ(body_hex(cb::AddTransientBlock{4, -60, -13, 121}), body_hex(cb::BlockUpdate{4, -60, -13, 121}));
    EXPECT_EQ(body_hex(cb::AddTransientBlock{4, -60, -13, 4926}),
              "0000013fffff3fc4"
              "be26");
    // FallingBlockEntity.DATA_START_POS: accessor 8, serializer 10 (BLOCK_POS), the position as a long.
    u8 entry[16];
    const u8* end = cb::put_entity_block_pos(entry, entity_data::falling_block::kStartPos, pack_block_pos(4, -60, -13));
    ASSERT_EQ(end - entry, 10);
    EXPECT_EQ(entry[0], 8);
    EXPECT_EQ(entry[1], 10);
    EXPECT_EQ(load_be64(entry + 2), 0x0000013fffff3fc4ull);
    // A falling block's add_entity carries its block state where other entities carry 0.
    const std::string add = body_hex(cb::AddEntity{7, {1, 2}, entity_type::kFallingBlock, 0.5, 70.0, 0.5, 0, 0, 0, 0, 0, 0, 121});
    EXPECT_EQ(add.substr(add.size() - 2), "79");
}

TEST(Packets, ChunkBatchReceivedLayout) {
    const u8 body[] = {0x41, 0x48, 0x00, 0x00};  // 12.5f
    Reader r(body, sizeof body);
    EXPECT_EQ(sb::ChunkBatchReceived::decode(r).desired_chunks_per_tick, 12.5f);
    EXPECT_TRUE(r.done());
    Reader cut(body, 3);
    sb::ChunkBatchReceived::decode(cut);
    EXPECT_FALSE(cut.ok);
}

TEST(Packets, ContainerClickLayout) {
    // ServerboundContainerClickPacket: container id, state id, slot (short), button (byte), input,
    // the changed slots as a map short -> HashedStack, the carried HashedStack. A HashedStack is
    // optional: item, count, a map component type -> hash (int) and a set of removed types.
    u8 buf[128];
    u8* p = put_varint(put_varint(buf, 0), 300);  // container 0, state 300
    p = put_be16(p, static_cast<u16>(-999));
    p = put_u8(p, 0xFF);  // button -1
    p = put_varint(p, sb::ContainerClick::kQuickCraft);
    p = put_varint(p, 2);  // two changed slots
    p = put_be16(p, 36);
    p = put_u8(p, 0);  // 36: empty
    p = put_be16(p, 9);
    p = put_u8(p, 1);                        // 9: a stack
    p = put_varint(put_varint(p, 1084), 1);  //    diamond helmet x 1
    p = put_varint(p, 2);                    //    two hashed components
    p = put_be32(put_varint(p, 3), 0xDEADBEEF);
    p = put_be32(put_varint(p, 13), 0x01020304);
    p = put_varint(put_varint(p, 1), 11);  //    one removed: lore
    p = put_u8(p, 1);                      // carried: stone x 64, no components
    p = put_varint(put_varint(put_varint(put_varint(p, 1), 64), 0), 0);
    Reader r(buf, static_cast<usize>(p - buf));
    sb::ContainerClick c;
    sb::ContainerClick::decode(r, c);
    EXPECT_TRUE(r.done());
    EXPECT_EQ(c.container_id, 0u);
    EXPECT_EQ(c.state_id, 300u);
    EXPECT_EQ(c.slot, -999);
    EXPECT_EQ(c.button, -1);
    EXPECT_EQ(c.input, sb::ContainerClick::kQuickCraft);
    ASSERT_EQ(c.changed_count, 2u);
    EXPECT_EQ(c.changed[0].slot, 36);
    EXPECT_FALSE(c.changed[0].stack.present);
    EXPECT_EQ(c.changed[1].slot, 9);
    EXPECT_TRUE(c.changed[1].stack.present);
    EXPECT_EQ(c.changed[1].stack.item, 1084u);
    EXPECT_EQ(c.changed[1].stack.count, 1);
    EXPECT_EQ(c.changed[1].stack.components, 3u);
    EXPECT_EQ(c.changed[1].stack.highest_type, 13u);
    EXPECT_TRUE(c.carried.present);
    EXPECT_EQ(c.carried.item, 1u);
    EXPECT_EQ(c.carried.count, 64);
    EXPECT_EQ(c.carried.components, 0u);

    // An input outside the enum is PICKUP (ByIdMap ... ZERO); more than 128 changed slots is an error.
    const u8 odd[] = {0, 1, 0, 5, 0, 99, 0, 0};
    Reader ro(odd, sizeof odd);
    sb::ContainerClick::decode(ro, c);
    EXPECT_TRUE(ro.done());
    EXPECT_EQ(c.input, sb::ContainerClick::kPickup);
    EXPECT_EQ(c.slot, 5);
    const u8 many[] = {0, 1, 0, 5, 0, 0, 0x81, 0x01};
    Reader rm(many, sizeof many);
    sb::ContainerClick::decode(rm, c);
    EXPECT_FALSE(rm.ok);
    const u8 hashes[] = {1, 1, 1, 0x81, 0x02};  // 257 hashed components
    Reader rh(hashes, sizeof hashes);
    sb::HashedStack::decode(rh);
    EXPECT_FALSE(rh.ok);

    const u8 pick[] = {0, 0, 0, 0, 0, 0, 0x10, 0x05, 1};
    Reader rp(pick, sizeof pick);
    const auto pk = sb::PickItemFromBlock::decode(rp);
    EXPECT_TRUE(rp.done());
    EXPECT_EQ(pk.pos, (BlockPos{0, 5, 1}));
    EXPECT_TRUE(pk.include_data);
}
