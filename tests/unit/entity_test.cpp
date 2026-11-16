// The entity table and the tracker, tested from the outside: a miniature client decodes exactly
// what a player's connection would carry and its picture of the world is compared with the truth.
#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "entity/store.hpp"
#include "entity/tracker.hpp"
#include "item/stack.hpp"
#include "proto/frame.hpp"
#include "proto/packets.hpp"

#include PROTO_GEN_ITEMS_HPP

using namespace proto;
using entity::Store;
using entity::Tracker;
namespace flag = entity::flag;
namespace cbid = ids::play::cb;
namespace et = proto::entity_type;
namespace ed = proto::entity_data;

namespace {

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

// An item stack as a client reads it (ItemStack.OPTIONAL_STREAM_CODEC). Of the components only
// `damage` is understood - enough to see that a patch travels.
struct Stack {
    u32 item = 0, count = 0;
    i32 damage = -1;  // -1: no patch
    bool operator==(const Stack&) const = default;
    static Stack read(Reader& r) {
        Stack s;
        s.count = r.varint();
        if (!s.count) return s;
        s.item = r.varint();
        const u32 added = r.varint(), removed = r.varint();
        EXPECT_EQ(removed, 0u);
        EXPECT_LE(added, 1u);
        if (added) {
            EXPECT_EQ(r.varint(), proto::gen::component::kDamage);
            s.damage = static_cast<i32>(r.varint());
        }
        return s;
    }
};
void PrintTo(const Stack& s, std::ostream* os) { *os << "item " << s.item << " x " << s.count << " damage " << s.damage; }

struct Seen {  // one entity as a client holds it
    Uuid uuid;
    u32 type = 0;
    double base[3] = {};  // VecDeltaCodec base
    double vel[3] = {};
    float yaw = 0, pitch = 0;
    i8 head = 0;
    bool on_ground = false;
    bool bundled = false;
    std::map<u32, double> data;      // accessor -> value (all the values in use fit a double)
    std::vector<u32> steps;          // tick counts of the path steps, in arrival order
    std::map<u32, Stack> equipment;  // EquipmentSlot -> what it holds, for the slots that hold something
    Stack item;                      // the ITEM_STACK value among its data, if it has one
};

// What ClientPacketListener does with entity packets, as far as position, rotation and data go.
struct Client {
    i32 own_id = 0;
    i32 threshold = kNoCompression;
    std::map<i32, Seen> seen;
    std::map<u32, double> own_data;
    std::vector<std::string> log;  // "name id" per packet, in order
    bool in_bundle = false;

    static void read_data(Reader& r, std::map<u32, double>& into, Stack* item = nullptr) {
        for (;;) {
            const u32 accessor = r.u8v();
            if (accessor == 0xFF || !r.ok) return;
            const u32 ser = r.varint();
            if (ser == ed::serializer::kByte)
                into[accessor] = r.u8v();
            else if (ser == ed::serializer::kBoolean)
                into[accessor] = r.boolean();
            else if (ser == ed::serializer::kItemStack && item) {
                *item = Stack::read(r);
                into[accessor] = item->count;
            } else if (ser == ed::serializer::kFloat)
                into[accessor] = r.f32();
            else if (ser == ed::serializer::kPose || ser == ed::serializer::kHumanoidArm || ser == ed::serializer::kInt)
                into[accessor] = r.varint();
            else
                FAIL() << "serializer " << ser;
        }
    }
    static void delta(Seen& e, Reader& r) {  // VecDeltaCodec.decode + setBase
        for (double& b : e.base) {
            const i16 d = static_cast<i16>(r.be16());
            if (d) b = (std::floor(b * 4096.0 + 0.5) + d) / 4096.0;
        }
    }

    void feed(base::ByteBuf& buf) {
        const u8* p = buf.data;
        const u8* end = buf.data + buf.len;
        while (p < end) {
            std::span<const u8> body;
            u32 used = 0;
            ASSERT_EQ(frame_split(p, end, kMaxFrame, body, used), FrameStatus::Ok);
            p += used;
            if (threshold >= 0) {  // [0][body] below the threshold, [size][zlib] from it on
                const usize framed = body.size();
                ASSERT_TRUE(frame_inflate(body, threshold, body));
                ASSERT_EQ(body.size() + 1 == framed, body.size() < static_cast<usize>(threshold))
                    << "stored and compressed frames are on the wrong sides of the threshold";
            }
            Reader r(body);
            const u32 id = r.varint();
            if (id == cbid::kBundleDelimiter) {
                in_bundle = !in_bundle;
                log.push_back("bundle");
                ASSERT_TRUE(r.done());
                continue;
            }
            if (id == cbid::kRemoveEntities) {
                ASSERT_EQ(r.varint(), 1u);
                const i32 eid = static_cast<i32>(r.varint());
                ASSERT_EQ(seen.erase(eid), 1u) << "removed an entity the client never had: " << eid;
                log.push_back("remove " + std::to_string(eid));
                ASSERT_TRUE(r.done());
                continue;
            }
            if (id == cbid::kEntityEvent) {  // the one packet here whose entity id is a plain int
                const i32 who = static_cast<i32>(r.be32());
                const u8 event = r.u8v();
                ASSERT_EQ(seen.count(who), 1u) << "an event for an unknown entity";
                if (event == cb::EntityEvent::kSwapHands) {  // LivingEntity.handleEntityEvent(55)
                    auto& eq = seen[who].equipment;
                    const Stack main = eq.count(0) ? eq[0] : Stack(), off = eq.count(1) ? eq[1] : Stack();
                    eq.erase(0), eq.erase(1);
                    if (off.count) eq[0] = off;
                    if (main.count) eq[1] = main;
                }
                log.push_back("event " + std::to_string(who) + " " + std::to_string(event));
                ASSERT_TRUE(r.done());
                continue;
            }
            const i32 eid = static_cast<i32>(r.varint());
            if (id == cbid::kAddEntity) {
                ASSERT_NE(eid, own_id) << "shown itself";
                ASSERT_EQ(seen.count(eid), 0u) << "added twice: " << eid;
                Seen& e = seen[eid];
                e.uuid = r.uuid();
                e.type = r.varint();
                for (double& b : e.base) b = r.f64();
                read_lp_vec3(r, e.vel);
                e.pitch = unpack_degrees(r.u8v());
                e.yaw = unpack_degrees(r.u8v());
                e.head = static_cast<i8>(r.u8v());
                EXPECT_EQ(r.varint(), 0u);
                e.bundled = in_bundle;
                log.push_back("add " + std::to_string(eid));
            } else if (id == cbid::kSetEntityData) {
                if (eid == own_id) {
                    read_data(r, own_data);
                } else {
                    ASSERT_EQ(seen.count(eid), 1u) << "data for an unknown entity";
                    read_data(r, seen[eid].data, &seen[eid].item);
                }
                log.push_back("data " + std::to_string(eid));
            } else if (id == cbid::kSetEquipment) {  // handleSetEquipment
                ASSERT_EQ(seen.count(eid), 1u) << "equipment of an unknown entity";
                std::string line = "equip " + std::to_string(eid);
                for (bool more = true; more && r.ok;) {
                    const u8 slot = r.u8v();
                    more = slot & 0x80;
                    const Stack stack = Stack::read(r);
                    if (stack.count)
                        seen[eid].equipment[slot & 0x7F] = stack;
                    else
                        seen[eid].equipment.erase(slot & 0x7F);
                    line += " " + std::to_string(slot & 0x7F);
                }
                log.push_back(line);
            } else if (id == cbid::kTakeItemEntity) {
                ASSERT_EQ(seen.count(eid), 1u) << "an unknown item was taken";
                const u32 collector = r.varint(), amount = r.varint();
                log.push_back("take " + std::to_string(eid) + " by " + std::to_string(collector) + " x " +
                              std::to_string(amount));
            } else {
                ASSERT_EQ(seen.count(eid), 1u) << "packet " << id << " for an unknown entity " << eid;
                Seen& e = seen[eid];
                if (id == cbid::kMoveEntityPos || id == cbid::kMoveEntityPosRot) {
                    const u32 props = r.varint();
                    ASSERT_LE(props >> 1, 1u);
                    if (props >> 1) e.steps.push_back(r.varint());
                    delta(e, r);
                    if (id == cbid::kMoveEntityPosRot) {
                        e.yaw = unpack_degrees(r.u8v());
                        e.pitch = unpack_degrees(r.u8v());
                    }
                    e.on_ground = props & 1;
                    log.push_back((id == cbid::kMoveEntityPos ? "pos " : "posrot ") + std::to_string(eid));
                } else if (id == cbid::kMoveEntityRot) {
                    e.on_ground = r.boolean();
                    e.yaw = unpack_degrees(r.u8v());
                    e.pitch = unpack_degrees(r.u8v());
                    log.push_back("rot " + std::to_string(eid));
                } else if (id == cbid::kEntityPositionSync) {
                    const u32 kind = r.varint();
                    ASSERT_LE(kind, 1u);
                    if (kind) {
                        ASSERT_EQ(r.varint(), 1u);
                    }
                    for (double& b : e.base) b = r.f64();
                    if (kind) e.steps.push_back(r.varint());
                    e.yaw = r.f32();
                    e.pitch = r.f32();
                    e.on_ground = r.boolean();
                    log.push_back("sync " + std::to_string(eid));
                } else if (id == cbid::kRotateHead) {
                    e.head = static_cast<i8>(r.u8v());
                    log.push_back("head " + std::to_string(eid));
                } else if (id == cbid::kSetEntityMotion) {
                    read_lp_vec3(r, e.vel);
                    log.push_back("motion " + std::to_string(eid));
                } else {
                    FAIL() << "unexpected packet " << id;
                }
            }
            ASSERT_TRUE(r.done()) << "packet " << id;
        }
        ASSERT_FALSE(in_bundle) << "a bundle was left open";
        buf.clear();
    }
};

// The part of server::Game that owns players: one output buffer and one client per player row,
// kept in step with the store's row moves.
struct World {
    Store store;
    Tracker tracker;
    std::vector<std::unique_ptr<base::ByteBuf>> bufs;
    std::vector<base::ByteBuf*> outs;
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<u32> views;  // per player row, in chunks
    u64 next_uuid = 1;

    i32 threshold;

    explicit World(i32 threshold_ = kNoCompression, u32 range_percent = 100)
        : tracker(store, threshold_, range_percent), threshold(threshold_) {}

    u32 add_player(double x, double y, double z, u32 view_chunks) {
        Store::Moves mv;
        const u32 p = tracker.spawn(et::kPlayer, true, Uuid{7, next_uuid++}, x, y, z, 0, 0, mv);
        EXPECT_EQ(p, bufs.size());
        bufs.push_back(std::make_unique<base::ByteBuf>(1024));
        outs.push_back(bufs.back().get());
        clients.push_back(std::make_unique<Client>());
        clients.back()->own_id = store.net_id(p);
        clients.back()->threshold = threshold;
        views.push_back(view_chunks);
        tracker.set_view_distance(p, view_chunks);
        return p;
    }
    u32 add(u32 type, double x, double y, double z) {
        Store::Moves mv;
        return tracker.spawn(type, false, Uuid{9, next_uuid++}, x, y, z, 0, 0, mv);
    }
    void remove(u32 e) {
        const u32 players = store.players();
        Store::Moves mv;
        tracker.despawn(e, outs.data(), mv);
        if (e < players) {  // as Game::remove_player: the last player takes the row
            const u32 last = players - 1;
            if (e != last) {
                bufs[e] = std::move(bufs[last]);
                clients[e] = std::move(clients[last]);
                views[e] = views[last];
            }
            bufs.pop_back();
            clients.pop_back();
            views.pop_back();
            outs.clear();
            for (auto& b : bufs) outs.push_back(b.get());
        }
    }
    void move(u32 e, double x, double y, double z) {
        store.x[e] = x;
        store.y[e] = y;
        store.z[e] = z;
        if (e < store.players()) store.flags[e] |= flag::kMoved;  // what a movement packet does
    }
    // One server tick: the tracker, then the players' own tick (the head follows the body).
    void tick() {
        tracker.tick(outs.data());
        for (u32 p = 0; p < store.players(); ++p) {
            store.head_yaw[p] = store.yaw[p];
            clients[p]->feed(*bufs[p]);
        }
    }
    void ticks(int n) {
        for (int i = 0; i < n; ++i) tick();
    }
    Client& client(u32 p) { return *clients[p]; }
    const Seen& view(u32 viewer, u32 e) { return clients[viewer]->seen.at(store.net_id(e)); }
    bool shown(u32 viewer, u32 e) { return clients[viewer]->seen.count(store.net_id(e)) != 0; }
    // ChunkMap.TrackedEntity.updatePlayer, the scalar way.
    bool in_range(u32 viewer, u32 e) const {
        if (viewer == e) return false;
        const double dx = store.x[viewer] - store.x[e], dz = store.z[viewer] - store.z[e];
        const double range = std::min<double>(et::kInfo[store.type[e]].tracking_range * 16, views[viewer] * 16);
        return !(dx * dx + dz * dz > range * range);
    }
};

std::vector<std::string> take(std::vector<std::string>& log) {
    std::vector<std::string> out;
    out.swap(log);
    return out;
}
using Log = std::vector<std::string>;

constexpr u32 kHealth = ed::living_entity::kHealth, kFlags = ed::entity::kSharedFlags;

}  // namespace

// ---------------------------------------------------------------------------- store

TEST(EntityStore, PlayersStayInFrontAndMovesAreReported) {
    Store s;
    Store::Moves mv;
    const u32 m0 = s.create(et::kPig, false, Uuid{0, 1}, 1, 0, 0, 0, 0, mv);
    const u32 m1 = s.create(et::kCow, false, Uuid{0, 2}, 2, 0, 0, 0, 0, mv);
    EXPECT_EQ(m0, 0u);
    EXPECT_EQ(m1, 1u);
    EXPECT_EQ(mv.n, 0u);
    const entity::Handle pig = s.handle(m0), cow = s.handle(m1);

    // A player takes row 0; the pig that was there goes to the end.
    const u32 p0 = s.create(et::kPlayer, true, Uuid{0, 3}, 3, 0, 0, 0, 0, mv);
    EXPECT_EQ(p0, 0u);
    ASSERT_EQ(mv.n, 1u);
    EXPECT_EQ(mv.m[0].from, 0u);
    EXPECT_EQ(mv.m[0].to, 2u);
    EXPECT_EQ(s.players(), 1u);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.resolve(pig), 2u);
    EXPECT_EQ(s.resolve(cow), 1u);
    EXPECT_EQ(s.x[s.resolve(pig)], 1.0);
    EXPECT_EQ(s.type[s.resolve(cow)], et::kCow);
    const u32 p1 = s.create(et::kPlayer, true, Uuid{0, 4}, 4, 0, 0, 0, 0, mv);
    EXPECT_EQ(p1, 1u);
    EXPECT_EQ(s.resolve(cow), 3u);
    const entity::Handle h0 = s.handle(p0), h1 = s.handle(p1);

    // Network ids are slot + 1, in creation order here, and find their rows wherever those went.
    EXPECT_EQ(s.net_id(s.resolve(pig)), 1);
    EXPECT_EQ(s.net_id(p0), 3);
    EXPECT_EQ(s.by_net_id(1), s.resolve(pig));
    EXPECT_EQ(s.by_net_id(4), p1);
    EXPECT_EQ(s.by_net_id(5), entity::kNone);
    EXPECT_EQ(s.by_net_id(0), entity::kNone);

    // Removing the first player: the last player fills its row, the last entity fills that one.
    s.destroy(p0, mv);
    ASSERT_EQ(mv.n, 2u);
    EXPECT_EQ(mv.m[0].from, 1u);
    EXPECT_EQ(mv.m[0].to, 0u);
    EXPECT_EQ(mv.m[1].from, 3u);
    EXPECT_EQ(mv.m[1].to, 1u);
    EXPECT_EQ(s.players(), 1u);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.resolve(h0), entity::kNone);
    EXPECT_EQ(s.resolve(h1), 0u);
    EXPECT_EQ(s.x[0], 4.0);
    EXPECT_EQ(s.resolve(cow), 1u);
    EXPECT_EQ(s.resolve(pig), 2u);
    EXPECT_EQ(s.by_net_id(3), entity::kNone);

    // The freed slot is reused, under a new generation: the old handle stays dead.
    const u32 m2 = s.create(et::kZombie, false, Uuid{0, 5}, 5, 0, 0, 0, 0, mv);
    EXPECT_EQ(s.net_id(m2), 3);
    EXPECT_NE(s.handle(m2), h0);
    EXPECT_EQ(s.resolve(h0), entity::kNone);
    EXPECT_EQ(s.resolve(s.handle(m2)), m2);
    EXPECT_EQ(s.resolve(0), entity::kNone);
    EXPECT_LT(s.handle(m2), 1ull << 53) << "a handle must survive being a Lua number";

    // Type facts land in the flags.
    EXPECT_TRUE(s.flags[m2] & flag::kLiving);
    EXPECT_TRUE(s.flags[m2] & flag::kTrackDeltas);
    EXPECT_FALSE(s.flags[0] & flag::kTrackDeltas);  // the player
    const u32 item = s.create(et::kItem, false, Uuid{0, 6}, 0, 0, 0, 0, 0, mv);
    EXPECT_FALSE(s.flags[item] & flag::kLiving);
}

TEST(EntityStore, GrowsAndKeepsEverything) {
    Store s;
    Store::Moves mv;
    std::vector<entity::Handle> handles;
    for (u32 i = 0; i < 5000; ++i) {
        const bool player = i % 7 == 0;
        const u32 e = s.create(player ? et::kPlayer : et::kPig, player, Uuid{1, i}, i, -static_cast<double>(i), 0.5, 0, 0, mv);
        handles.push_back(s.handle(e));
    }
    EXPECT_EQ(s.capacity() % 64, 0u);
    for (u32 i = 0; i < 5000; ++i) {
        const u32 e = s.resolve(handles[i]);
        ASSERT_NE(e, entity::kNone);
        EXPECT_EQ(s.x[e], static_cast<double>(i));
        EXPECT_EQ(s.uuid[e], (Uuid{1, i}));
        EXPECT_EQ(e < s.players(), i % 7 == 0);
        EXPECT_EQ(s.columns().x, s.x);
    }
    std::mt19937 rng(3);
    while (s.size() > 100) {
        const u32 e = rng() % s.size();
        const entity::Handle h = s.handle(e);
        s.destroy(e, mv);
        EXPECT_EQ(s.resolve(h), entity::kNone);
    }
    u32 live = 0;
    for (u32 i = 0; i < 5000; ++i) {
        const u32 e = s.resolve(handles[i]);
        if (e == entity::kNone) continue;
        ++live;
        EXPECT_EQ(s.x[e], static_cast<double>(i));
        EXPECT_EQ(e < s.players(), i % 7 == 0);
    }
    EXPECT_EQ(live, 100u);
}

// ---------------------------------------------------------------------------- tracker

TEST(Tracker, PlayersAreShownEachOtherLikeVanilla) {
    World w;
    const u32 a = w.add_player(8.5, -60, 8.5, 10);
    w.tick();
    // Alone: its own data, once (health and nothing else: no skin layers were set).
    EXPECT_EQ(take(w.client(a).log), (Log{"data 1"}));
    EXPECT_EQ(w.client(a).own_data, (std::map<u32, double>{{ed::living_entity::kHealth, 20.0}}));
    w.ticks(3);
    EXPECT_TRUE(w.client(a).log.empty());

    const u32 b = w.add_player(8.5, -60, 1.5, 10);
    w.store.set_skin_parts(b, 0x7F);
    w.tick();
    // The bundle, then b's first update: a zero move (tick 0 of its life is a forced position)
    // and its dirty data - what a vanilla server sent in the same situation.
    EXPECT_EQ(take(w.client(a).log), (Log{"bundle", "add 2", "data 2", "bundle", "pos 2", "data 2"}));
    EXPECT_EQ(take(w.client(b).log), (Log{"bundle", "add 1", "data 1", "bundle", "data 2"}));
    const Seen& sb = w.view(a, b);
    EXPECT_TRUE(sb.bundled);
    EXPECT_EQ(sb.type, et::kPlayer);
    EXPECT_EQ(sb.uuid, (Uuid{7, 2}));
    EXPECT_EQ(sb.base[0], 8.5);
    EXPECT_EQ(sb.base[2], 1.5);
    EXPECT_TRUE(sb.steps.empty()) << "the first move is a plain delta, not a step";
    EXPECT_EQ(sb.data,
              (std::map<u32, double>{{ed::living_entity::kHealth, 20.0}, {ed::avatar::kPlayerModeCustomisation, 127.0}}));
    EXPECT_EQ(w.client(b).own_data, sb.data);
    EXPECT_EQ(w.tracker.viewers(a), 1u);
    EXPECT_TRUE(w.tracker.sees(a, b) && w.tracker.sees(b, a));
    EXPECT_FALSE(w.tracker.sees(a, a));

    // Standing still says nothing...
    w.ticks(40);
    EXPECT_TRUE(w.client(a).log.empty());
    // ...except the position every 60 ticks of each entity's life (a's, then b's).
    w.ticks(19);
    EXPECT_EQ(take(w.client(b).log), (Log{"pos 1"}));
    w.ticks(4);
    EXPECT_EQ(take(w.client(a).log), (Log{"pos 2"}));
}

TEST(Tracker, MovementIsDeltasOfOneStepEveryOtherTick) {
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 10), b = w.add_player(2.5, 64, 0.5, 10);
    w.ticks(4);
    take(w.client(a).log);
    // b walks 1/4 block per tick. Its update interval is 2: every second tick, a's client gets
    // one step that took 2 ticks.
    double x = 2.5;
    for (int i = 0; i < 20; ++i) {
        x += 0.25;
        w.move(b, x, 64, 0.5);
        w.tick();
    }
    w.ticks(2);
    const Seen& sb = w.view(a, b);
    EXPECT_EQ(sb.base[0], x);
    EXPECT_EQ(take(w.client(a).log), Log(11, "pos 2"));  // 20 ticks of walking, and the last quarter block after them
    EXPECT_EQ(sb.steps, std::vector<u32>(11, 2u));
    EXPECT_FALSE(sb.on_ground);

    // Landing: the on-ground flag changed, which vanilla answers with an absolute position.
    w.store.flags[b] |= flag::kOnGround;
    w.move(b, x + 0.125, 64, 0.5);
    w.ticks(2);
    EXPECT_EQ(take(w.client(a).log), (Log{"sync 2"}));
    EXPECT_TRUE(sb.on_ground);
    EXPECT_EQ(sb.base[0], x + 0.125);

    // A delta holds +-8 blocks per axis (32767 / 4096); beyond that, absolute again. Exactly -8 fits.
    w.move(b, x + 0.125 - 8.0, 64, 0.5);
    w.ticks(2);
    EXPECT_EQ(take(w.client(a).log), (Log{"pos 2"}));
    w.move(b, x + 0.125, 64, 0.5);  // +8 is one unit too many
    w.ticks(2);
    EXPECT_EQ(take(w.client(a).log), (Log{"sync 2"}));
    EXPECT_EQ(sb.base[0], x + 0.125);

    // Positions that are not multiples of 1/4096: the client is off by less than half a unit,
    // and the error never accumulates (each delta is taken between rounded absolutes).
    double px = x;
    std::mt19937 rng(11);
    for (int i = 0; i < 300; ++i) {
        px += (static_cast<double>(rng() % 2001) - 1000.0) / 7919.0;
        w.move(b, px, 64 + (rng() % 100) / 333.0, 0.5 + (rng() % 100) / 77.0);
        w.tick();
    }
    w.ticks(2);
    EXPECT_NEAR(sb.base[0], px, 1.0 / 8192 + 1e-12);
    EXPECT_NEAR(sb.base[1], w.store.y[b], 1.0 / 8192 + 1e-12);
    EXPECT_NEAR(sb.base[2], w.store.z[b], 1.0 / 8192 + 1e-12);
    EXPECT_EQ(sb.base[0], std::floor(px * 4096.0 + 0.5) / 4096.0);
}

TEST(Tracker, AbsolutePositionEvery400Updates) {
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 10), b = w.add_player(2.5, 64, 0.5, 10);
    w.ticks(2);
    take(w.client(a).log);
    int syncs = 0, moves = 0;
    for (int i = 0; i < 2000; ++i) {
        w.move(b, 2.5 + ((i + 1) % 64) / 64.0, 64, 0.5);
        w.tick();
        for (const std::string& l : take(w.client(a).log)) (l == "sync 2" ? syncs : moves) += 1;
    }
    // 2000 ticks are 1000 updates, each with a new position; every 401st of them is absolute.
    EXPECT_EQ(syncs, 2);
    EXPECT_EQ(moves, 998);
}

TEST(Tracker, RotationAndTheHeadOneTickBehind) {
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 10), b = w.add_player(2.5, 64, 0.5, 10);
    w.ticks(4);  // b's updates now fall on even ticks of this loop
    take(w.client(a).log);
    const Seen& sb = w.view(a, b);

    // The packets of the vanilla capture: a turn of (10, 5) degrees per tick for six ticks.
    Log all;
    for (int i = 1; i <= 6; ++i) {
        w.store.yaw[b] = 10.0f * i;
        w.store.pitch[b] = 5.0f * i;
        w.tick();
        for (auto& l : take(w.client(a).log))
            all.push_back(l + " yaw=" + std::to_string(static_cast<int>(pack_degrees(sb.yaw))) +
                          " head=" + std::to_string(sb.head));
    }
    w.ticks(2);
    for (auto& l : take(w.client(a).log))
        all.push_back(l + " yaw=" + std::to_string(static_cast<int>(pack_degrees(sb.yaw))) + " head=" + std::to_string(sb.head));
    // Body at once; the head is what the body was one tick earlier, because the player's own
    // tick (which copies it) runs after the tracker.
    // These are the bytes the vanilla server sent for the same six turns: yaw 7, 21, 35, 42 and
    // head 14, 28, 42 (10, 30, 50, 60 and 20, 40, 60 degrees).
    EXPECT_EQ(all, (Log{"rot 2 yaw=7 head=0", "rot 2 yaw=21 head=14", "head 2 yaw=21 head=14", "rot 2 yaw=35 head=28",
                        "head 2 yaw=35 head=28", "rot 2 yaw=42 head=42", "head 2 yaw=42 head=42"}));
    EXPECT_EQ(sb.pitch, unpack_degrees(21));

    // Moving and turning in one update is one packet.
    w.store.yaw[b] = 90.0f;
    w.store.pitch[b] = -20.0f;
    w.move(b, 2.75, 64, 0.5);
    w.ticks(2);
    EXPECT_EQ(take(w.client(a).log), (Log{"posrot 2"}));
    EXPECT_EQ(pack_degrees(sb.yaw), 0x40);
    EXPECT_EQ(pack_degrees(sb.pitch), 0xF1);
    w.ticks(2);
    EXPECT_EQ(take(w.client(a).log), (Log{"head 2"}));
    EXPECT_EQ(sb.head, 0x40);
}

TEST(Tracker, DataGoesToViewersAndToThePlayerItself) {
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 10), b = w.add_player(2.5, 64, 0.5, 10);
    w.ticks(3);
    take(w.client(a).log);
    take(w.client(b).log);

    // A dirty value does not wait for the update interval.
    w.store.set_shared_flag(b, entity::shared_flag::kShiftKeyDown, true);
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"data 2"}));
    EXPECT_EQ(take(w.client(b).log), (Log{"data 2"}));
    EXPECT_EQ(w.view(a, b).data.at(ed::entity::kSharedFlags), 2.0);
    EXPECT_EQ(w.client(b).own_data.at(ed::entity::kSharedFlags), 2.0);
    w.store.set_shared_flag(b, entity::shared_flag::kShiftKeyDown, true);  // no change: not dirty
    w.store.set_pose(b, pose::kCrouching);
    w.store.set_main_hand(b, entity::kMainHandLeft);
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"data 2"}));
    EXPECT_EQ(w.view(a, b).data.at(ed::entity::kPose), static_cast<double>(pose::kCrouching));
    EXPECT_EQ(w.view(a, b).data.at(ed::avatar::kPlayerMainHand), 0.0);
    w.ticks(5);
    EXPECT_TRUE(w.client(a).log.empty());

    // Someone new is told what differs from a fresh entity - and nothing that does not.
    w.store.set_pose(b, pose::kStanding);
    w.tick();
    const u32 c = w.add_player(1.5, 64, 0.5, 10);
    w.tick();
    EXPECT_EQ(w.view(c, b).data, (std::map<u32, double>{{kFlags, 2.0}, {kHealth, 20.0}, {ed::avatar::kPlayerMainHand, 0.0}}));
    EXPECT_EQ(w.view(c, a).data, (std::map<u32, double>{{kHealth, 20.0}}));
}

TEST(Tracker, RangeIsHorizontalInclusiveAndTheSmallerOfViewAndType) {
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 2);    // sees 32 blocks
    const u32 b = w.add_player(0.5, 64, 32.5, 10);  // sees 160
    w.tick();
    EXPECT_TRUE(w.shown(a, b)) << "exactly 32 blocks is inside";
    EXPECT_TRUE(w.shown(b, a));
    w.move(b, 0.5, 64, 32.5 + 1.0 / 1024);
    w.tick();
    EXPECT_FALSE(w.shown(a, b));
    EXPECT_TRUE(w.shown(b, a)) << "each side uses its own view distance";
    EXPECT_EQ(take(w.client(a).log).back(), "remove 2");
    w.move(b, 0.5, 300, 32.5);  // height does not count
    w.tick();
    EXPECT_TRUE(w.shown(a, b));
    EXPECT_EQ(w.view(a, b).base[1], 300.0);

    // A pig is shown 10 chunks far, an item 6: the type's range when it is the smaller one.
    const u32 pig = w.add(et::kPig, 0.5, 64, -159.5), item = w.add(et::kItem, 0.5, 64, 32.5 + 96.0);
    w.tick();
    EXPECT_FALSE(w.shown(a, pig));
    EXPECT_FALSE(w.shown(b, pig));  // 192 away
    EXPECT_TRUE(w.shown(b, item));  // exactly 96
    w.move(b, 0.5, 64, 0.5);
    w.tick();
    EXPECT_TRUE(w.shown(b, pig));  // exactly 160
    EXPECT_FALSE(w.shown(b, item));
    EXPECT_EQ(w.tracker.viewers(pig), 1u);

    // entity-broadcast-range-percentage scales the type's range (integer arithmetic, as vanilla).
    World half(kNoCompression, 50);
    const u32 p = half.add_player(0.5, 64, 0.5, 32);
    const u32 near = half.add(et::kPig, 80.5, 64, 0.5), far = half.add(et::kPig, 80.75, 64, 0.5);
    half.tick();
    EXPECT_TRUE(half.shown(p, near));
    EXPECT_FALSE(half.shown(p, far));
}

// ChunkMap.isChunkTracked: an entity that stands in a chunk the player has not been sent yet is
// not shown - and is, when the player next says where it is after the chunk has gone out.
TEST(Tracker, WhatStandsInAChunkStillToComeIsNotShown) {
    World w;
    const u32 a = w.add_player(8.5, 64, 8.5, 4);
    const u32 b = w.add_player(40.5, 64, 8.5, 4);     // chunk (2, 0)
    const u32 cow = w.add(et::kCow, 24.5, 64, 8.5);   // chunk (1, 0)
    const u32 pig = w.add(et::kPig, 12.5, 64, 12.5);  // chunk (0, 0)
    // a's window: 11 x 11 chunks around (0, 0); chunks (1, 0) and (2, 0) are waiting to be sent.
    constexpr u32 kSide = 11;
    u64 bits[2] = {};
    const auto set = [&](i32 cx, i32 cz, bool on) {
        const u32 i = static_cast<u32>(cz + 5) * kSide + static_cast<u32>(cx + 5);
        bits[i >> 6] = on ? bits[i >> 6] | 1ull << (i & 63) : bits[i >> 6] & ~(1ull << (i & 63));
    };
    set(1, 0, true);
    set(2, 0, true);
    entity::PendingChunks pending[2] = {{bits, -5, -5, kSide, 2}, {}};
    w.tracker.set_pending_chunks(pending);
    w.tick();
    EXPECT_TRUE(w.shown(a, pig));
    EXPECT_FALSE(w.shown(a, cow));
    EXPECT_FALSE(w.shown(a, b));
    EXPECT_TRUE(w.shown(b, a)) << "b has every chunk";
    EXPECT_TRUE(w.shown(b, cow));
    // The cow's chunk goes out. Nothing is looked at again until a says where it is...
    set(1, 0, false);
    pending[0].count = 1;
    w.ticks(3);
    EXPECT_FALSE(w.shown(a, cow));
    w.move(a, 8.5, 64, 8.5);
    w.tick();
    EXPECT_TRUE(w.shown(a, cow));
    EXPECT_FALSE(w.shown(a, b));
    // ...and the same for the player in the last chunk.
    set(2, 0, false);
    pending[0].count = 0;
    w.move(a, 8.5, 64, 8.5);
    w.tick();
    EXPECT_TRUE(w.shown(a, b));
    // An entity that walks into a chunk that waits goes out of sight like one that leaves.
    set(1, 0, true);
    pending[0].count = 1;
    w.move(pig, 24.5, 64, 12.5);
    w.tick();
    EXPECT_FALSE(w.shown(a, pig));
    EXPECT_TRUE(w.shown(a, cow)) << "what is shown stays until it is looked at again";
    w.tracker.set_pending_chunks(nullptr);
}

TEST(Tracker, OnlyWhatMovedIsLookedAtAgain) {
    // Vanilla re-evaluates a pair when the player sends a movement packet or the entity changes
    // section - not when a mob shuffles inside its section.
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 2);
    const u32 pig = w.add(et::kPig, 34.5, 64, 0.5);  // 34 blocks: out of sight
    w.tick();
    EXPECT_FALSE(w.shown(a, pig));
    w.move(pig, 32.25, 64, 0.5);  // in range now, same section (x 32..47), and the player is idle
    w.ticks(3);
    EXPECT_FALSE(w.shown(a, pig));
    w.store.flags[a] |= flag::kMoved;  // any movement packet of the player: its whole row is redone
    w.tick();
    EXPECT_TRUE(w.shown(a, pig));
    w.move(pig, 40.5, 64, 0.5);  // out of range, same section
    w.ticks(3);
    EXPECT_TRUE(w.shown(a, pig));
    w.move(pig, 48.5, 64, 0.5);  // a new section: the pig is tested against everyone
    w.tick();
    EXPECT_FALSE(w.shown(a, pig));
}

TEST(Tracker, MobsAreSteppedEveryThirdTickWithVelocityItemsArePlain) {
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 10);
    const u32 pig = w.add(et::kPig, 4.5, 64, 0.5), item = w.add(et::kItem, 0.5, 64, 4.5);
    w.tick();
    const i32 pig_id = w.store.net_id(pig), item_id = w.store.net_id(item);
    const std::string sp = std::to_string(pig_id), si = std::to_string(item_id);
    // Nothing non-default to say about either: the bundles hold add_entity alone.
    EXPECT_EQ(take(w.client(a).log),
              (Log{"bundle", "add " + sp, "bundle", "bundle", "add " + si, "bundle", "pos " + sp, "pos " + si, "data 1"}));

    w.store.vx[pig] = 0.1;
    Log seen;
    for (int i = 0; i < 9; ++i) {
        w.move(pig, w.store.x[pig] + 0.1, 64, 0.5);
        w.tick();
        for (auto& l : take(w.client(a).log)) seen.push_back(l);
    }
    // Update interval 3 (tick 0 was the spawn tick): motion once - it only changed once - then moves.
    EXPECT_EQ(seen, (Log{"motion " + sp, "pos " + sp, "pos " + sp, "pos " + sp}));
    EXPECT_EQ(w.view(a, pig).steps, std::vector<u32>(3, 3u));
    EXPECT_NEAR(w.view(a, pig).vel[0], 0.1, 1e-4);
    EXPECT_NEAR(w.view(a, pig).base[0], w.store.x[pig], 1.0 / 8192 + 1e-12);

    // An item: interval 20, no steps (it is not a LivingEntity), velocity tracked too.
    w.store.vy[item] = -0.04;
    w.move(item, 0.5, 63.5, 4.5);
    seen.clear();
    for (int i = 0; i < 20; ++i) {
        w.tick();
        for (auto& l : take(w.client(a).log)) seen.push_back(l);
    }
    EXPECT_EQ(seen, (Log{"motion " + si, "pos " + si}));
    EXPECT_TRUE(w.view(a, item).steps.empty());
    EXPECT_EQ(w.view(a, item).base[1], 63.5);

    // An impulse (Entity.needsSync) does not wait for the interval; Entity.syncVelocity neither.
    w.store.vy[item] = 0.3;
    w.store.flags[item] |= flag::kNeedsSync;
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"motion " + si}));
    EXPECT_FALSE(w.store.flags[item] & flag::kNeedsSync);
    w.store.flags[item] |= flag::kSyncVelocity;
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"motion " + si}));
    EXPECT_FALSE(w.store.flags[item] & flag::kSyncVelocity);
}

TEST(Tracker, DespawnTellsViewersAndRowsKeepTheirPairs) {
    World w;
    // Three players within sight, one far away, two pigs near the first three.
    const u32 a = w.add_player(0.5, 64, 0.5, 4), b = w.add_player(2.5, 64, 0.5, 4), c = w.add_player(4.5, 64, 0.5, 4);
    const u32 far = w.add_player(1000.5, 64, 0.5, 4);
    const u32 pig1 = w.add(et::kPig, 6.5, 64, 0.5), pig2 = w.add(et::kPig, 1002.5, 64, 0.5);
    w.ticks(2);
    const i32 id_b = w.store.net_id(b), id_c = w.store.net_id(c), id_far = w.store.net_id(far), id_p1 = w.store.net_id(pig1),
              id_p2 = w.store.net_id(pig2);
    Client* client_c = w.clients[c].get();
    Client* client_far = w.clients[far].get();
    EXPECT_EQ(w.client(a).seen.size(), 3u);
    EXPECT_EQ(client_far->seen.size(), 1u);
    take(w.client(a).log);

    // a leaves: b and c are told; `far` (the last player) takes row 0, the last pig its row.
    const entity::Handle hb = w.store.handle(b), hc = w.store.handle(c), hfar = w.store.handle(far), hp1 = w.store.handle(pig1),
                         hp2 = w.store.handle(pig2);
    w.remove(a);
    EXPECT_EQ(w.store.resolve(hfar), 0u);
    EXPECT_EQ(w.store.players(), 3u);
    w.tick();
    const u32 nb = w.store.resolve(hb), nc = w.store.resolve(hc), nfar = w.store.resolve(hfar), np1 = w.store.resolve(hp1),
              np2 = w.store.resolve(hp2);
    EXPECT_EQ(w.clients[nc].get(), client_c);
    EXPECT_EQ(w.clients[nfar].get(), client_far);
    const auto ids = [](const Client& cl) {
        std::set<i32> s;
        for (const auto& [id, e] : cl.seen) s.insert(id);
        return s;
    };
    EXPECT_EQ(ids(w.client(nb)), (std::set<i32>{id_c, id_p1}));
    EXPECT_EQ(ids(w.client(nc)), (std::set<i32>{id_b, id_p1}));
    EXPECT_EQ(ids(w.client(nfar)), (std::set<i32>{id_p2}));
    EXPECT_TRUE(w.tracker.sees(nb, nc) && w.tracker.sees(nb, np1) && !w.tracker.sees(nb, np2) && !w.tracker.sees(nb, nfar));
    EXPECT_TRUE(w.tracker.sees(nfar, np2) && !w.tracker.sees(nfar, nb));
    EXPECT_EQ(w.tracker.viewers(np1), 2u);
    EXPECT_EQ(w.tracker.viewers(nc), 1u);

    // The bits moved with their rows: updates still reach the right clients.
    w.move(np1, 6.75, 64, 0.5);
    w.move(np2, 1002.75, 64, 0.5);
    w.ticks(3);
    EXPECT_EQ(w.client(nb).seen.at(id_p1).base[0], 6.75);
    EXPECT_EQ(w.client(nc).seen.at(id_p1).base[0], 6.75);
    EXPECT_EQ(w.client(nfar).seen.at(id_p2).base[0], 1002.75);

    // A pig dies; its id is taken by the next entity, which the same clients then see as new.
    w.remove(np1);
    w.tick();
    EXPECT_EQ(ids(w.client(w.store.resolve(hb))), (std::set<i32>{id_c}));
    const u32 cow = w.add(et::kCow, 3.5, 64, 0.5);
    EXPECT_EQ(w.store.net_id(cow), id_p1);
    w.tick();
    EXPECT_EQ(w.client(w.store.resolve(hb)).seen.at(id_p1).type, et::kCow);
    (void)id_far;
}

TEST(Tracker, RandomWorldMatchesBruteForce) {
    // Hundreds of entities over several words of the bit rows, spawning, dying and moving at
    // random; every player is "moved" each tick so that visibility is plain geometry. After each
    // tick every client must hold exactly the entities in range, and after things settle, at
    // exactly the place they are.
    for (const i32 threshold : {kNoCompression, 256, 16}) {
        World w(threshold);
        std::mt19937 rng(static_cast<u32>(threshold) + 99);
        const auto coord = [&] {
            return static_cast<double>(static_cast<i32>(rng() % 6400) - 3200) / 16.0;
        };  // +-200, multiples of 1/16
        std::vector<entity::Handle> all;
        for (int i = 0; i < 70; ++i) all.push_back(w.store.handle(w.add_player(coord(), 64, coord(), 1 + rng() % 8)));
        for (int i = 0; i < 300; ++i) all.push_back(w.store.handle(w.add(i % 3 ? et::kPig : et::kItem, coord(), 64, coord())));
        const u32 kinds[] = {et::kPig, et::kItem, et::kZombie, et::kArmorStand};

        for (int tick = 0; tick < 150; ++tick) {
            const bool calm = tick >= 120;  // the last 30 ticks: nothing changes any more, updates drain
            if (!calm) {
                for (int k = 0; k < 6; ++k) {  // some die, some are born
                    const u32 e = w.store.resolve(all[rng() % all.size()]);
                    if (e != entity::kNone && w.store.size() > 200) w.remove(e);
                    if (rng() % 2)
                        all.push_back(w.store.handle(w.add(kinds[rng() % 4], coord(), 64, coord())));
                    else if (w.store.players() < 90)
                        all.push_back(w.store.handle(w.add_player(coord(), 64, coord(), 1 + rng() % 8)));
                }
                for (u32 e = 0; e < w.store.size(); ++e) {
                    if (rng() % 3 == 0)
                        w.move(e, w.store.x[e] + (static_cast<i32>(rng() % 97) - 48) / 16.0, 64,
                               w.store.z[e] + (static_cast<i32>(rng() % 97) - 48) / 16.0);
                    if (rng() % 9 == 0) w.store.yaw[e] = static_cast<float>(rng() % 360);
                }
            }
            for (u32 e = 0; e < w.store.size(); ++e) w.store.flags[e] |= flag::kMoved;
            w.tick();
            if (::testing::Test::HasFatalFailure()) return;

            for (u32 p = 0; p < w.store.players(); ++p) {
                std::set<i32> expect, have;
                for (u32 e = 0; e < w.store.size(); ++e) {
                    ASSERT_EQ(w.tracker.sees(p, e), w.in_range(p, e)) << "row " << p << " -> " << e << " at tick " << tick;
                    if (w.in_range(p, e)) expect.insert(w.store.net_id(e));
                }
                for (const auto& [id, seen] : w.client(p).seen) have.insert(id);
                ASSERT_EQ(have, expect) << "player row " << p << " at tick " << tick;
            }
        }
        // When things have settled, every client has everything it sees exactly where it is.
        for (u32 p = 0; p < w.store.players(); ++p) {
            for (const auto& [id, s] : w.client(p).seen) {
                const u32 e = w.store.by_net_id(id);
                ASSERT_NE(e, entity::kNone);
                EXPECT_EQ(s.base[0], w.store.x[e]) << "entity " << id << " as seen by row " << p;
                EXPECT_EQ(s.base[2], w.store.z[e]);
                EXPECT_EQ(s.uuid, w.store.uuid[e]);
                EXPECT_EQ(s.type, w.store.type[e]);
                if (w.store.flags[e] & flag::kLiving) {
                    EXPECT_EQ(pack_degrees(s.yaw), pack_degrees(w.store.yaw[e]));
                }
            }
        }
        u32 pairs = 0;
        for (u32 e = 0; e < w.store.size(); ++e) pairs += w.tracker.viewers(e);
        u32 held = 0;
        for (u32 p = 0; p < w.store.players(); ++p) held += static_cast<u32>(w.client(p).seen.size());
        EXPECT_EQ(pairs, held);
        EXPECT_GT(pairs, 500u) << "the test world is too sparse to mean anything";
    }
}

TEST(Tracker, GrowthKeepsEveryPair) {
    // More entities than the first allocation holds and more players than the first rows: the
    // bit rows are laid out again, and nobody may lose or gain a pair by it.
    World w;
    const u32 a = w.add_player(0.5, 64, 0.5, 10);
    for (int i = 0; i < 40; ++i) w.add(et::kPig, 1.5 + i, 64, 0.5);
    w.tick();
    EXPECT_EQ(w.client(a).seen.size(), 40u);
    const u32 capacity = w.store.capacity();
    for (int i = 0; i < 3000; ++i) w.add(et::kPig, (i % 100) - 50.5, 64, (i / 100) - 15.5);
    for (int i = 0; i < 150; ++i) w.add_player(0.5 + i * 0.25, 64, 2.5, 10);  // each pushes a pig to the end
    EXPECT_GT(w.store.capacity(), capacity);
    w.tick();
    for (u32 p = 0; p < w.store.players(); ++p) {
        u32 expect = 0;
        for (u32 e = 0; e < w.store.size(); ++e) {
            ASSERT_EQ(w.tracker.sees(p, e), w.in_range(p, e)) << p << " -> " << e;
            expect += w.in_range(p, e);
        }
        ASSERT_EQ(w.client(p).seen.size(), expect);
        ASSERT_EQ(expect, 3040u + 150u);
    }
    for (u32 e = 0; e < w.store.size(); ++e) ASSERT_EQ(w.tracker.viewers(e), e < w.store.players() ? 150u : 151u);
}

TEST(Tracker, VisibilityBitsAreTheGeometry) {
    // The SIMD scan against the scalar rule, pair by pair - with pairs placed exactly on the
    // boundary, where "<=" and "<" (or a fused multiply-add) would disagree.
    World w;
    std::mt19937 rng(5);
    const auto coord = [&] { return (static_cast<i32>(rng() % 801) - 400) * 0.5; };
    for (int i = 0; i < 100; ++i) w.add_player(coord(), 64, coord(), 1 + rng() % 12);
    for (int i = 0; i < 700; ++i) w.add(i % 2 ? et::kPig : et::kArrow, coord(), 64, coord());
    for (u32 p = 0; p < 20; ++p) {  // one arrow (shown 64 blocks far) and one pig (160) on each of these players' limits
        w.add(et::kArrow, w.store.x[p] + std::min(64.0, w.views[p] * 16.0), 64, w.store.z[p]);
        w.add(et::kPig, w.store.x[p], 64, w.store.z[p] - std::min(160.0, w.views[p] * 16.0));
    }
    w.tick();
    u32 on_boundary = 0, visible = 0;
    for (u32 p = 0; p < w.store.players(); ++p) {
        for (u32 e = 0; e < w.store.size(); ++e) {
            const double dx = w.store.x[p] - w.store.x[e], dz = w.store.z[p] - w.store.z[e];
            const double range = std::min<double>(et::kInfo[w.store.type[e]].tracking_range * 16, w.views[p] * 16);
            on_boundary += e != p && dx * dx + dz * dz == range * range;
            visible += w.tracker.sees(p, e);
            ASSERT_EQ(w.tracker.sees(p, e), w.in_range(p, e)) << p << " -> " << e;
        }
    }
    EXPECT_GE(on_boundary, 40u);
    EXPECT_GT(visible, 1000u);
}

// ---------------------------------------------------------------------------- equipment, items

namespace {

namespace equip = item::equip;
namespace it = proto::gen::item;

struct Worn {
    item::ItemStack slot[equip::kCount];
};

// A stack with a damage component, as a client would send it.
item::ItemStack damaged(u32 item_id, u32 damage) {
    u8 buf[32];
    u8* p = put_varint(put_varint(buf, 1), item_id);  // count, item
    p = put_varint(put_varint(p, 1), 0);              // one added, none removed
    p = put_varint(p, proto::gen::component::kDamage);
    p = put_varint(put_varint(p, varint_size(damage)), damage);  // length, value
    Reader r(buf, static_cast<usize>(p - buf));
    item::ItemStack s;
    EXPECT_EQ(item::read_untrusted(r, s), item::Verdict::kOk);
    return s;
}

}  // namespace

TEST(Tracker, EquipmentGoesToViewersChangedSlotsOnly) {
    World w;
    const u32 a = w.add_player(0, 64, 0, 10), b = w.add_player(5, 64, 0, 10);
    w.ticks(2);
    const std::string ida = std::to_string(w.store.net_id(a)), idb = std::to_string(w.store.net_id(b));
    take(w.client(a).log), take(w.client(b).log);

    Worn worn;
    w.tracker.show_equipment(b, worn.slot, w.outs.data());  // nothing held, nothing said
    w.tick();
    EXPECT_TRUE(w.client(a).log.empty());

    worn.slot[equip::kMainhand] = item::ItemStack(it::kStone, 64);
    worn.slot[equip::kHead] = damaged(it::kDiamondHelmet, 7);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"equip " + idb + " 0 5"}));  // one packet, slots in EquipmentSlot order
    EXPECT_TRUE(w.client(b).log.empty());                              // not to the wearer: its client knows
    EXPECT_EQ(w.view(a, b).equipment.at(equip::kMainhand), (Stack{it::kStone, 64, -1}));
    EXPECT_EQ(w.view(a, b).equipment.at(equip::kHead), (Stack{it::kDiamondHelmet, 1, 7}));  // components and all

    w.tracker.show_equipment(b, worn.slot, w.outs.data());  // the same again: silence
    w.tick();
    EXPECT_TRUE(w.client(a).log.empty());

    worn.slot[equip::kMainhand].shrink(1);  // a count is a change (ItemStack.matches)
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"equip " + idb + " 0"}));
    EXPECT_EQ(w.view(a, b).equipment.at(equip::kMainhand).count, 63u);

    worn.slot[equip::kHead] = {};  // taken off: an empty stack for that slot
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"equip " + idb + " 5"}));
    EXPECT_EQ(w.view(a, b).equipment.count(equip::kHead), 0u);
}

TEST(Tracker, SwappedHandsAreAnEvent) {
    World w;
    const u32 a = w.add_player(0, 64, 0, 10), b = w.add_player(5, 64, 0, 10);
    w.ticks(2);
    const std::string idb = std::to_string(w.store.net_id(b));
    Worn worn;
    worn.slot[equip::kMainhand] = item::ItemStack(it::kStone, 3);
    worn.slot[equip::kOffhand] = item::ItemStack(it::kShield, 1);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    take(w.client(a).log);

    std::swap(worn.slot[equip::kMainhand], worn.slot[equip::kOffhand]);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"event " + idb + " 55"}));  // no stacks on the wire: the viewer swaps its own
    EXPECT_EQ(w.view(a, b).equipment.at(equip::kMainhand).item, it::kShield);
    EXPECT_EQ(w.view(a, b).equipment.at(equip::kOffhand).item, it::kStone);

    // A swap together with another change: the event, then the other slot.
    std::swap(worn.slot[equip::kMainhand], worn.slot[equip::kOffhand]);
    worn.slot[equip::kFeet] = item::ItemStack(it::kLeatherBoots, 1);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"event " + idb + " 55", "equip " + idb + " 2"}));

    // Not a swap: one hand emptied, the other changed.
    worn.slot[equip::kMainhand] = {};
    worn.slot[equip::kOffhand] = item::ItemStack(it::kStone, 3);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"equip " + idb + " 0 1"}));
    // A swap with one hand empty is still a swap.
    std::swap(worn.slot[equip::kMainhand], worn.slot[equip::kOffhand]);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"event " + idb + " 55"}));
    EXPECT_EQ(w.view(a, b).equipment.count(equip::kOffhand), 0u);
    EXPECT_EQ(w.view(a, b).equipment.at(equip::kMainhand).item, it::kStone);
}

TEST(Tracker, NewcomersAreShownEquipmentWithTheEntityAndRowsKeepTheirs) {
    World w;
    const u32 a = w.add_player(0, 64, 0, 10);
    u32 b = w.add_player(5, 64, 0, 10);
    w.ticks(2);
    Worn worn;
    worn.slot[equip::kOffhand] = item::ItemStack(it::kShield, 1);
    worn.slot[equip::kChest] = item::ItemStack(it::kElytra, 1);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();

    // The first player leaves: B's row moves into its place, and what B wears moves along.
    const i32 id_b = w.store.net_id(b);
    w.remove(a);
    b = w.store.by_net_id(id_b);
    ASSERT_EQ(b, 0u);
    const u32 c = w.add_player(3, 64, 3, 10);
    w.tick();
    // The bundle: add_entity, its data, then every slot that holds something (ServerEntity.sendPairingData).
    const std::string sb = std::to_string(id_b);
    EXPECT_EQ(take(w.client(c).log), (Log{"bundle", "add " + sb, "data " + sb, "equip " + sb + " 1 4", "bundle",
                                          "data " + std::to_string(w.store.net_id(c))}));
    EXPECT_EQ(w.view(c, b).equipment.at(equip::kOffhand).item, it::kShield);
    EXPECT_EQ(w.view(c, b).equipment.at(equip::kChest).item, it::kElytra);
    EXPECT_EQ(w.view(c, b).equipment.size(), 2u);
    // C wears nothing: no equipment packet about it reaches B.
    for (const std::string& line : w.client(b).log) EXPECT_NE(line.rfind("equip", 0), 0u) << line;

    // Everything off: later newcomers get no equipment packet at all.
    w.tracker.show_equipment(b, Worn().slot, w.outs.data());
    w.tick();
    const u32 d = w.add_player(-3, 64, 3, 10);
    w.tick();
    for (const std::string& line : w.client(d).log) EXPECT_NE(line.rfind("equip", 0), 0u) << line;
    // Leaving with equipment on gives the patch references back (checked by the store at exit).
    worn.slot[equip::kHead] = damaged(it::kDiamondHelmet, 3);
    w.tracker.show_equipment(b, worn.slot, w.outs.data());
    w.tick();
    w.remove(b);
    w.tick();
}

TEST(Tracker, ItemEntitiesCarryTheirStack) {
    World w;
    const u32 a = w.add_player(0, 64, 0, 10);
    w.ticks(2);
    take(w.client(a).log);
    u32 e = w.add(et::kItem, 2, 64, 2);
    w.store.set_item(e, damaged(it::kDiamondSword, 40));
    w.store.set_no_gravity(e, true);
    const std::string id = std::to_string(w.store.net_id(e));
    w.tick();
    // The bundle holds every non-default value; the first sendChanges then sends them again as
    // "dirty", exactly as a vanilla entity whose constructor set its data does.
    EXPECT_EQ(take(w.client(a).log), (Log{"bundle", "add " + id, "data " + id, "bundle", "pos " + id, "data " + id}));
    EXPECT_EQ(w.view(a, e).item, (Stack{it::kDiamondSword, 1, 40}));
    EXPECT_EQ(w.view(a, e).data.at(ed::entity::kNoGravity), 1.0);
    EXPECT_EQ(w.view(a, e).data.count(ed::item_entity::kItem), 1u);

    w.store.set_item(e, item::ItemStack(it::kStone, 5));  // ItemEntity.setItem: viewers hear at once
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"data " + id}));
    EXPECT_EQ(w.view(a, e).item, (Stack{it::kStone, 5, -1}));
    w.store.item[e].shrink(2);  // changed in place, as a partial pickup does: nobody is told
    w.ticks(3);
    EXPECT_TRUE(take(w.client(a).log).empty());
    EXPECT_EQ(w.view(a, e).item.count, 5u);

    // sendToTrackingPlayers: to those who see the entity, and nobody else.
    const u32 far = w.add_player(900, 64, 900, 10);
    e = w.store.by_net_id(std::stoi(id));  // the new player took the item's row
    w.ticks(2);
    take(w.client(a).log), take(w.client(far).log);
    base::ByteBuf framed;
    write_packet(framed, w.threshold, cb::TakeItemEntity{w.store.net_id(e), w.store.net_id(a), 3});
    w.tracker.broadcast(e, {framed.data, framed.len}, w.outs.data());
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"take " + id + " by " + std::to_string(w.store.net_id(a)) + " x 3"}));
    EXPECT_TRUE(w.client(far).log.empty());

    w.remove(e);
    w.tick();
    EXPECT_EQ(take(w.client(a).log), (Log{"remove " + id}));
}

TEST(Grid, SmallCellsForSmallQuestions) {
    // The same grid with small cells (the game keeps items in one of 2 blocks): here 4.
    entity::BasicGrid<2> grid;
    static_assert(entity::BasicGrid<2>::cell_of(3) == 0 && entity::BasicGrid<2>::cell_of(4) == 1 &&
                  entity::BasicGrid<2>::cell_of(-1) == -1);
    static_assert(entity::Grid::cell_of(63) == 0 && entity::Grid::cell_of(64) == 1);
    const u32 a = grid.add(0, 0, 100), b = grid.add(0, 0, 101), c = grid.add(5, -3, 102);
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u);
    EXPECT_EQ(c, 0u);
    grid.add(40, 40, 103);  // another region
    const auto found = [&](i32 x0, i32 z0, i32 x1, i32 z1) {
        std::set<u32> out;
        grid.for_each(x0, z0, x1, z1, [&](u32 slot) { EXPECT_TRUE(out.insert(slot).second); });
        return out;
    };
    EXPECT_EQ(found(0, 0, 0, 0), (std::set<u32>{100, 101}));
    EXPECT_EQ(found(-1, -4, 5, 0), (std::set<u32>{100, 101, 102}));
    EXPECT_EQ(found(1, 1, 4, 4), (std::set<u32>{}));
    EXPECT_EQ(found(-100, -100, 100, 100), (std::set<u32>{100, 101, 102, 103}));
    EXPECT_EQ(grid.remove(0, 0, a), 101u);  // the cell's last entry fills the gap, and says so
    EXPECT_EQ(found(0, 0, 0, 0), (std::set<u32>{101}));
    EXPECT_EQ(grid.remove(0, 0, 0), ~0u);
    EXPECT_EQ(grid.size(), 2u);
}
