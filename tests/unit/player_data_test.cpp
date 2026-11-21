// SPDX-License-Identifier: GPL-3.0-or-later
// What is kept of a player: its file's bytes, written out by hand from the layout in
// server/player_data.hpp and never by the code under test - and the small files such bytes
// travel in, between the tick thread and the workers.
#include <gtest/gtest.h>

#include <unistd.h>

#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

#include "base/workers.hpp"
#include "proto/wire.hpp"
#include "server/files.hpp"
#include "server/player_data.hpp"

#include PROTO_GEN_ITEMS_HPP

using namespace server;
namespace comp = proto::gen::component;
namespace it = proto::gen::item;

namespace {

using Bytes = std::vector<u8>;

Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
Bytes vi(u32 v) {
    u8 buf[8];
    return {buf, proto::put_varint(buf, v)};
}
Bytes be64(u64 v) {
    Bytes out(8);
    for (u32 i = 0; i < 8; ++i) out[i] = static_cast<u8>(v >> (56 - 8 * i));
    return out;
}
Bytes be32(u32 v) { return {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)}; }
Bytes f64(double v) { return be64(std::bit_cast<u64>(v)); }
Bytes f32(float v) { return be32(std::bit_cast<u32>(v)); }

const Bytes kEmpty = vi(0);
Bytes plain(u32 item, u32 count) { return vi(count) + vi(item) + vi(0) + vi(0); }
// A wooden spear that has been used once: one component added (damage, a VarInt), none removed.
const Bytes kUsedSpear = vi(1) + vi(it::kWoodenSpear) + vi(1) + vi(0) + vi(comp::kDamage) + vi(1) + vi(1);

// The head of a file: version, flags, position, rotation, selected slot.
Bytes head(u8 version, u8 flags, double x, double y, double z, float yaw, float pitch, u8 selected) {
    return Bytes{version, flags} + f64(x) + f64(y) + f64(z) + f32(yaw) + f32(pitch) + Bytes{selected};
}
// ...and `n` stacks, of which those named are not empty.
Bytes stacks(u32 n, std::initializer_list<std::pair<u32, Bytes>> filled) {
    Bytes out = vi(n);
    for (u32 i = 0; i < n; ++i) {
        const Bytes* stack = &kEmpty;
        for (const auto& [slot, bytes] : filled)
            if (slot == i) stack = &bytes;
        out = out + *stack;
    }
    return out;
}

Bytes written(const PlayerData& d) {
    base::ByteBuf out;
    write_player(out, d);
    return {out.data, out.data + out.len};
}

item::ItemStack stack_of(const Bytes& in) {
    proto::Reader r(in.data(), in.size());
    item::ItemStack s;
    EXPECT_EQ(item::read_untrusted(r, s), item::Verdict::kOk);
    EXPECT_TRUE(r.done());
    return s;
}

struct TempDir {
    std::string path;
    TempDir() {
        char name[] = "/tmp/player-test-XXXXXX";
        path = ::mkdtemp(name);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

Bytes on_disk(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

// ---------------------------------------------------------------------------- the file's bytes

TEST(PlayerData, IsWrittenAsTheLayoutSays) {
    PlayerData d;
    d.x = -12.5;
    d.y = 70.0625;
    d.z = 300.25;
    d.yaw = -135.5f;
    d.pitch = 33.25f;
    d.on_ground = true;
    d.flying = false;
    d.selected = 3;
    d.items[0] = item::ItemStack(it::kDirt, 64);
    d.items[inv::kHead] = stack_of(kUsedSpear);
    d.items[inv::kOffhand] = item::ItemStack(it::kStone, 2);

    const Bytes expected =
        head(1, 1, -12.5, 70.0625, 300.25, -135.5f, 33.25f, 3) +
        stacks(inv::kSize, {{0, plain(it::kDirt, 64)}, {inv::kHead, kUsedSpear}, {inv::kOffhand, plain(it::kStone, 2)}});
    EXPECT_EQ(written(d), expected);

    PlayerData back;
    ASSERT_TRUE(read_player(expected, back));
    EXPECT_EQ(back.x, d.x);
    EXPECT_EQ(back.y, d.y);
    EXPECT_EQ(back.z, d.z);
    EXPECT_EQ(back.yaw, d.yaw);
    EXPECT_EQ(back.pitch, d.pitch);
    EXPECT_TRUE(back.on_ground);
    EXPECT_FALSE(back.flying);
    EXPECT_EQ(back.selected, 3);
    for (u32 i = 0; i < inv::kSize; ++i) EXPECT_TRUE(item::matches(back.items[i], d.items[i])) << "slot " << i;
    EXPECT_EQ(written(back), expected);
}

TEST(PlayerData, FlagsAndFewerSlots) {
    // Flying, in the air; a file that names only the first two slots leaves the others empty.
    PlayerData d;
    d.items[5] = item::ItemStack(it::kStone, 1);  // must not survive the read
    ASSERT_TRUE(read_player(head(1, 2, 0, 100, 0, 0, -90.0f, 8) + stacks(2, {{1, plain(it::kStone, 7)}}), d));
    EXPECT_FALSE(d.on_ground);
    EXPECT_TRUE(d.flying);
    EXPECT_EQ(d.selected, 8);
    EXPECT_TRUE(d.items[0].empty());
    EXPECT_EQ(d.items[1].item(), static_cast<u32>(it::kStone));
    EXPECT_EQ(d.items[1].count(), 7u);
    for (u32 i = 2; i < inv::kSize; ++i) EXPECT_TRUE(d.items[i].empty()) << "slot " << i;
    // No slots at all is a player with empty hands.
    EXPECT_TRUE(read_player(head(1, 3, 0, 0, 0, 0, 0, 0) + vi(0), d));
}

TEST(PlayerData, WhatIsNotAPlayerIsRefused) {
    PlayerData d;
    const Bytes none = vi(0);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    EXPECT_TRUE(read_player(head(1, 0, 3.0E7, -2.0E7, -3.0E7, 179.0f, 90.0f, 0) + none, d));  // the edges are places
    EXPECT_FALSE(read_player({}, d));
    EXPECT_FALSE(read_player(head(2, 0, 0, 0, 0, 0, 0, 0) + none, d)) << "a version this server does not know";
    EXPECT_FALSE(read_player(head(0, 0, 0, 0, 0, 0, 0, 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 4, 0, 0, 0, 0, 0, 0) + none, d)) << "a flag this version has not got";
    EXPECT_FALSE(read_player(head(1, 0, nan, 0, 0, 0, 0, 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 0, 0, inf, 0, 0, 0, 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 3.0000001E7, 0, 0, 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 0, 0, 2.0000001E7, 0, 0, 0, 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, std::numeric_limits<float>::infinity(), 0, 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, 90.5f, 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, std::nanf(""), 0) + none, d));
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, 0, 9) + none, d)) << "the hotbar has nine slots";
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, 0, 0) + stacks(inv::kSize + 1, {}), d)) << "more slots than an inventory has";
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, 0, 0) + none + Bytes{0}, d)) << "bytes after the end";
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, 0, 0), d)) << "cut off before the slots";
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, 0, 0) + vi(3) + kEmpty, d)) << "cut off among them";
    // A stack that is no stack: an item nobody has.
    EXPECT_FALSE(read_player(head(1, 0, 0, 0, 0, 0, 0, 0) + vi(1) + plain(0x0FFFFFFF, 1), d));
    Bytes cut = head(1, 0, 0, 0, 0, 0, 0, 0) + vi(1) + kUsedSpear;
    cut.pop_back();
    EXPECT_FALSE(read_player(cut, d));
}

// ---------------------------------------------------------------------------- small files

namespace {
struct Answer {
    u32 calls = 0;
    u64 tag = 0;
    bool found = false;
    Bytes bytes;
    static void take(void* ctx, u64 tag, std::span<const u8> bytes, bool found) {
        auto* a = static_cast<Answer*>(ctx);
        ++a->calls;
        a->tag = tag;
        a->found = found;
        a->bytes.assign(bytes.begin(), bytes.end());
    }
};
}  // namespace

TEST(Files, AreWrittenWholeAndReadBack) {
    for (const u32 threads : {0u, 2u}) {
        for (const bool sync : {false, true}) {
            TempDir dir;
            base::Workers workers(threads);
            Files files(workers, sync);
            const std::string path = dir.path + "/a.bin";

            Answer missing;
            files.read(path, &missing, 7, &Answer::take);
            EXPECT_EQ(files.busy(), 1u);
            files.finish();
            EXPECT_EQ(missing.calls, 1u);
            EXPECT_EQ(missing.tag, 7u);
            EXPECT_FALSE(missing.found);

            const Bytes first{1, 2, 3, 4, 5};
            files.write(path, first);
            files.finish();
            EXPECT_EQ(files.busy(), 0u);
            EXPECT_EQ(on_disk(path), first);
            EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));

            Answer there;
            files.read(path, &there, 8, &Answer::take);
            EXPECT_EQ(there.calls, 0u) << "nothing is in memory any more: the disk is asked";
            files.finish();
            EXPECT_EQ(there.calls, 1u);
            EXPECT_TRUE(there.found);
            EXPECT_EQ(there.bytes, first);

            // An empty file is a file.
            files.write(path, {});
            files.finish();
            Answer empty;
            files.read(path, &empty, 9, &Answer::take);
            files.finish();
            EXPECT_TRUE(empty.found);
            EXPECT_TRUE(empty.bytes.empty());
        }
    }
}

TEST(Files, AFileOnItsWayIsAnsweredFromMemory) {
    for (const u32 threads : {0u, 2u}) {
        TempDir dir;
        base::Workers workers(threads);
        Files files(workers, false);
        const std::string path = dir.path + "/b.bin", other = dir.path + "/c.bin";
        // Three writes of one file before any has come back: the last one is what a reader gets,
        // at once, and what the disk ends up with.
        files.write(path, Bytes{1});
        files.write(other, Bytes{9, 9});
        files.write(path, Bytes{2, 2});
        files.write(path, Bytes{3, 3, 3});
        Answer a;
        files.read(path, &a, 1, &Answer::take);
        EXPECT_EQ(a.calls, 1u);
        EXPECT_TRUE(a.found);
        EXPECT_EQ(a.bytes, (Bytes{3, 3, 3}));
        files.finish();
        EXPECT_EQ(on_disk(path), (Bytes{3, 3, 3}));
        EXPECT_EQ(on_disk(other), (Bytes{9, 9}));
        EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    }
}

TEST(Files, AWriteThatCannotBeDoneIsForgotten) {
    TempDir dir;
    base::Workers workers(1);
    Files files(workers, false);
    const std::string path = dir.path + "/no/such/dir/a.bin";
    files.write(path, Bytes{1});
    files.finish();
    EXPECT_FALSE(std::filesystem::exists(path));
    Answer a;
    files.read(path, &a, 1, &Answer::take);
    files.finish();
    EXPECT_EQ(a.calls, 1u);
    EXPECT_FALSE(a.found);
}
