// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/gen_pipeline.hpp"

#include <algorithm>
#include <bit>

namespace world {

namespace {

constexpr u64 kSweepEvery = 20;  // ticks between two looks at what is kept
// What a task is to the workers' scales, beside a chunk read from disk (2): the terrain of a
// chunk is the long one. (Chosen without a measurement.)
constexpr u32 kTerrainWeight = 8, kAreaWeight = 4;

u64 chunk_key(i32 cx, i32 cz) { return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz); }

// The earliest request's first; of one request's, in the order they were found.
bool later(const auto& a, const auto& b) { return a.seq != b.seq ? a.seq > b.seq : a.order > b.order; }

#ifndef NDEBUG
// Whether two chunks that should be the same chunk are.
bool same(const GenChunk& a, const GenChunk& b) {
    if (a.x != b.x || a.z != b.z || a.version != b.version || a.decorated != b.decorated || a.sections_count != b.sections_count)
        return false;
    for (u32 si = 0; si < a.sections_count; ++si)
        for (u32 i = 0; i < 4096; ++i)
            if (a.sections[si].get(i) != b.sections[si].get(i)) return false;
    if (!std::equal(std::begin(a.cells), std::begin(a.cells) + a.sections_count * 64, std::begin(b.cells))) return false;
    for (u32 map = 0; map < heightmap::kCount; ++map)
        if (!std::equal(std::begin(a.height[map]), std::end(a.height[map]), std::begin(b.height[map]))) return false;
    return a.marks == b.marks && a.block_ticks.size() == b.block_ticks.size() && a.fluid_ticks.size() == b.fluid_ticks.size() &&
           a.block_entities.size() == b.block_entities.size() && a.tags == b.tags;
}
#endif

}  // namespace

struct GenPipeline::AreaJob {
    GenChunk* area[16];
    i32 x0, z0;
    u32 pass;
    u32 node;
};

GenPipeline::GenPipeline(base::Workers& workers, ChunkGenerator& gen, Made made, void* ctx)
    : workers_(workers), gen_(gen), made_(made), ctx_(ctx) {
    // No more out than the workers will come to soon: what is handed to a worker cannot be
    // taken back when something more urgent turns up.
    cap_ = 2 * std::max<u32>(1, workers.threads());
}

GenPipeline::~GenPipeline() {
    DCHECK(running_ == 0);
    for (Node& n : nodes_)
        for (GenChunk* g : n.in) delete g;
}

// ---------------------------------------------------------------------------- nodes

u64 GenPipeline::key_of(u32 pass, i32 a, i32 b) {
    // (A chunk is within two million of the middle of the world, an area a quarter of that: 30 bits each.)
    return u64{pass} << 60 | (static_cast<u64>(static_cast<u32>(a)) & 0x3FFFFFFF) << 30 |
           (static_cast<u64>(static_cast<u32>(b)) & 0x3FFFFFFF);
}

GenPipeline::Node* GenPipeline::find(u32 pass, i32 a, i32 b) {
    const auto it = index_.find(key_of(pass, a, b));
    return it == index_.end() ? nullptr : &nodes_[it->second];
}

GenPipeline::Node& GenPipeline::node(u32 pass, i32 a, i32 b) {
    const u64 key = key_of(pass, a, b);
    const auto [it, added] = index_.try_emplace(key, 0);
    if (added) {
        if (free_.empty()) {
            it->second = static_cast<u32>(nodes_.size());
            nodes_.emplace_back();
        } else {
            it->second = free_.back();
            free_.pop_back();
        }
        Node& n = nodes_[it->second];
        n = Node{};
        n.index = it->second;
        n.key = key;
        n.a = a;
        n.b = b;
        n.pass = static_cast<u8>(pass);
        n.state = Node::kIdle;
        n.touched = now_;
    }
    return nodes_[it->second];
}

void GenPipeline::free_node(Node& n) {
    DCHECK(n.have == 0 && n.state != Node::kFree);
    index_.erase(n.key);
    n.state = Node::kFree;
    n.wanted = false;
    free_.push_back(n.index);
}

void GenPipeline::drop(Node& n) {
    for (u32 slots = n.have; slots; slots &= slots - 1) {
        GenChunk*& g = n.in[std::countr_zero(slots)];
        kept_bytes_ -= bytes_of(*g);
        --kept_chunks_;
        delete g;
        g = nullptr;
    }
    n.have = 0;
    free_node(n);
}

// ---------------------------------------------------------------------------- what is wanted

void GenPipeline::want(Node& n, u64 seq) {
    if (n.wanted) return;  // (requests come in the order of their turns: the first to want it is the earliest)
    n.wanted = true;
    n.seq = seq;
    if (n.state == Node::kRunning) return;  // what is asked of it is on its way
    if (n.pass == kOutput) return want(node(kPasses, n.a, n.b), seq);
    if (n.have == 0xFFFF) return queue(n, kWhole);
    if (n.pass == 1) {
        for (u32 slot = 0; slot < 16; ++slot)
            if (!((n.have | n.making) >> slot & 1)) queue(n, slot);
        return;
    }
    // The areas of the pass before that its chunks come from: two, half of it each.
    for (u32 slot = 0; slot < 16; ++slot) {
        if (n.have >> slot & 1) continue;
        const i32 x = area_x0(n.pass, n.a) + static_cast<i32>(slot & 3), z = area_z0(n.pass, n.b) + static_cast<i32>(slot >> 2);
        want(node(n.pass - 1u, area_a(n.pass - 1u, x), area_b(n.pass - 1u, z)), seq);
    }
}

void GenPipeline::queue(Node& n, u32 slot) {
    if (slot == kWhole) n.state = Node::kQueued;
    heap_.push_back({n.seq, order_++, n.key, n.index, slot});
    std::push_heap(heap_.begin(), heap_.end(), [](const Entry& a, const Entry& b) { return later(a, b); });
}

void GenPipeline::request(i32 cx, i32 cz, u64 seq) {
    DCHECK(!requests_.contains(chunk_key(cx, cz)));
    requests_[chunk_key(cx, cz)] = seq;
    Node& out = node(kOutput, area_a(kPasses, cx), area_b(kPasses, cz));
    const u32 bit = 1u << area_slot(kPasses, cx, cz);
    out.asked |= static_cast<u16>(bit);
    out.touched = now_;
    if (out.have & bit) return settle(out);  // made before, with its area
    want(out, seq);
    start();
}

bool GenPipeline::forget(i32 cx, i32 cz) {
    if (!requests_.erase(chunk_key(cx, cz))) return false;
    if (Node* out = find(kOutput, area_a(kPasses, cx), area_b(kPasses, cz)))
        out->asked &= static_cast<u16>(~(1u << area_slot(kPasses, cx, cz)));
    // What was wanted for this chunk alone is wanted no more: found out when something is next started.
    stale_ = true;
    return true;
}

// What is wanted, from the requests that are left: the same as if they had just been made, in
// their order.
void GenPipeline::rebuild() {
    stale_ = false;
    heap_.clear();
    for (Node& n : nodes_) {
        if (n.state == Node::kFree) continue;
        n.wanted = false;
        if (n.state == Node::kQueued) n.state = Node::kIdle;
    }
    std::vector<std::pair<u64, u64>> turns;
    turns.reserve(requests_.size());
    for (const auto& [key, seq] : requests_) turns.push_back({seq, key});
    std::sort(turns.begin(), turns.end());
    for (const auto& [seq, key] : turns) {
        const i32 cx = static_cast<i32>(key >> 32), cz = static_cast<i32>(key);
        want(node(kOutput, area_a(kPasses, cx), area_b(kPasses, cz)), seq);
    }
}

// ---------------------------------------------------------------------------- chunks on their way

// A chunk at the version before `pass`, to the area of that pass that holds it.
GenPipeline::Node& GenPipeline::give(u32 pass, GenChunk* g) {
    Node& n = node(pass, area_a(pass, g->x), area_b(pass, g->z));
    const u32 slot = area_slot(pass, g->x, g->z);
    DCHECK(g->version == pass - 1);
    if (n.state == Node::kRunning || (n.have >> slot & 1)) {
        // Made a second time, for the sake of the area beside this one: the one that is here - or
        // was, and is with a worker - is the same chunk.
        DCHECK(n.state == Node::kRunning || same(*n.in[slot], *g));
        delete g;
        return n;
    }
    n.in[slot] = g;
    n.have |= static_cast<u16>(1u << slot);
    n.touched = now_;
    kept_bytes_ += bytes_of(*g);
    ++kept_chunks_;
    return n;
}

// After chunks came to it: an area that is wanted and has all sixteen can be run; of finished
// chunks, the ones that are asked for go.
void GenPipeline::settle(Node& n) {
    if (n.pass != kOutput) {
        if (n.have == 0xFFFF && n.wanted && n.state == Node::kIdle) queue(n, kWhole);
        return;
    }
    for (u32 ready = n.asked & n.have; ready; ready &= ready - 1) {
        const u32 slot = static_cast<u32>(std::countr_zero(ready));
        GenChunk* g = n.in[slot];
        n.in[slot] = nullptr;
        n.have &= static_cast<u16>(~(1u << slot));
        n.asked &= static_cast<u16>(~(1u << slot));
        kept_bytes_ -= bytes_of(*g);
        --kept_chunks_;
        requests_.erase(chunk_key(g->x, g->z));
        made_(ctx_, g->x, g->z, g);
    }
    if (!n.asked) n.wanted = false;
    if (!n.asked && !n.have) free_node(n);
}

void GenPipeline::start() {
    if (stale_) rebuild();
    if (held_ || stopped_) return;
    while (running_ < cap_ && !heap_.empty()) {
        std::pop_heap(heap_.begin(), heap_.end(), [](const Entry& a, const Entry& b) { return later(a, b); });
        const Entry e = heap_.back();
        heap_.pop_back();
        Node& n = nodes_[e.node];
        if (n.state == Node::kFree || n.key != e.key || !n.wanted) continue;
        if (e.slot == kWhole) {
            if (n.state != Node::kQueued) continue;
            auto* job = new AreaJob{{}, area_x0(n.pass, n.a), area_z0(n.pass, n.b), n.pass, n.index};
            for (u32 i = 0; i < 16; ++i) {
                job->area[i] = n.in[i];
                kept_bytes_ -= bytes_of(*n.in[i]);
                n.in[i] = nullptr;
            }
            kept_chunks_ -= 16;
            n.have = 0;
            n.state = Node::kRunning;
            ++running_;
            workers_.submit({&area_work, &area_done, this, job, 0, 0, 0, 0}, kAreaWeight);
        } else {
            if (n.state == Node::kRunning || ((n.have | n.making) >> e.slot & 1)) continue;
            n.making |= static_cast<u16>(1u << e.slot);
            const i32 x = area_x0(1, n.a) + static_cast<i32>(e.slot & 3), z = area_z0(1, n.b) + static_cast<i32>(e.slot >> 2);
            ++running_;
            workers_.submit({&terrain_work, &terrain_done, this, new GenChunk(), static_cast<u32>(x), static_cast<u32>(z), 0, 0},
                            kTerrainWeight);
        }
    }
}

void GenPipeline::hold(bool on) {
    held_ = on;
    if (!on) start();
}

void GenPipeline::terrain_work(base::Task& t) {
    auto& p = *static_cast<GenPipeline*>(t.ctx);
    p.gen_.make_base(static_cast<i32>(static_cast<u32>(t.a)), static_cast<i32>(static_cast<u32>(t.b)),
                     *static_cast<GenChunk*>(t.data));
}

void GenPipeline::terrain_done(base::Task& t) {
    auto& p = *static_cast<GenPipeline*>(t.ctx);
    auto* g = static_cast<GenChunk*>(t.data);
    --p.running_;
    ++p.terrains_;
    Node& n = p.node(1, area_a(1, g->x), area_b(1, g->z));
    n.making &= static_cast<u16>(~(1u << area_slot(1, g->x, g->z)));
    p.settle(p.give(1, g));
    p.start();
}

void GenPipeline::area_work(base::Task& t) {
    auto& p = *static_cast<GenPipeline*>(t.ctx);
    auto* job = static_cast<AreaJob*>(t.data);
    p.gen_.decorate(job->area, job->x0, job->z0, job->pass);
}

void GenPipeline::area_done(base::Task& t) {
    auto& p = *static_cast<GenPipeline*>(t.ctx);
    auto* job = static_cast<AreaJob*>(t.data);
    --p.running_;
    ++p.areas_;
    {
        Node& ran = p.nodes_[job->node];
        DCHECK(ran.state == Node::kRunning && ran.pass == job->pass);
        // It has nothing left: whoever needs what it made again has it made again.
        p.free_node(ran);
    }
    // Its sixteen go on: to the two areas of the next pass, or - finished - to who asked.
    Node* next[16];
    u32 count = 0;
    for (GenChunk* g : job->area) {
        Node* to = &p.give(job->pass + 1, g);
        if (std::find(next, next + count, to) == next + count) next[count++] = to;
    }
    for (u32 i = 0; i < count; ++i) p.settle(*next[i]);
    delete job;
    p.start();
}

// ---------------------------------------------------------------------------- what is kept

void GenPipeline::tick(u64 now) {
    now_ = now;
    if (stale_) start();
    if (now_ - swept_ >= kSweepEvery) sweep();
}

void GenPipeline::sweep() {
    swept_ = now_;
    std::vector<std::pair<u64, u32>> kept;
    for (Node& n : nodes_) {
        // What a request waits for stays, and so does what a worker will come back to.
        if (n.state != Node::kIdle || n.wanted || n.making || n.asked) continue;
        if (!n.have) {
            free_node(n);
        } else if (now_ - n.touched >= keep_) {
            drop(n);
        } else if (kept_bytes_ > budget_) {
            kept.push_back({n.touched, n.index});
        }
    }
    if (kept_bytes_ <= budget_) return;
    std::sort(kept.begin(), kept.end());
    for (const auto& [touched, index] : kept) {
        if (kept_bytes_ <= budget_) break;
        drop(nodes_[index]);
    }
}

}  // namespace world
