#include "world/ticks.hpp"

#include <algorithm>

namespace world {

namespace {

constexpr u8 kVersion = 1;
constexpr u32 kEmpty = ~0u;  // no `what` is this: a type has 15 bits at most
constexpr u32 kRecordBytes = 9;

// ScheduledTick.DRAIN_ORDER, as "a comes after b": what std::push_heap wants for a min-heap.
ALWAYS_INLINE bool later(const ScheduledTick& a, const ScheduledTick& b) {
    return a.trigger != b.trigger ? a.trigger > b.trigger : a.order > b.order;
}

// ---- ticksPerPosition: linear probing, deleted by shifting back (no tombstones: a chunk's
// ticks come and go for as long as its machines run).
ALWAYS_INLINE u32 slot_of(u32 key, u32 cap) { return (key * 0x9E3779B1u) >> (32 - static_cast<u32>(std::countr_zero(cap))); }

bool set_has(const ChunkTicks& t, u32 key) {
    if (!t.set_n) return false;
    const u32 mask = t.set_cap - 1;
    for (u32 i = slot_of(key, t.set_cap);; i = (i + 1) & mask) {
        if (t.set[i] == key) return true;
        if (t.set[i] == kEmpty) return false;
    }
}

void set_grow(ChunkTicks& t, u32 cap) {
    u32* fresh = static_cast<u32*>(std::malloc(sizeof(u32) * cap));
    if (UNLIKELY(!fresh)) std::abort();
    std::fill_n(fresh, cap, kEmpty);
    const u32 mask = cap - 1;
    for (u32 i = 0; i < t.set_cap; ++i) {
        if (t.set[i] == kEmpty) continue;
        u32 at = slot_of(t.set[i], cap);
        while (fresh[at] != kEmpty) at = (at + 1) & mask;
        fresh[at] = t.set[i];
    }
    std::free(t.set);
    t.set = fresh;
    t.set_cap = cap;
}

// False if it was there.
bool set_add(ChunkTicks& t, u32 key) {
    if (UNLIKELY((t.set_n + 1) * 2 > t.set_cap)) set_grow(t, t.set_cap ? t.set_cap * 2 : 16);
    const u32 mask = t.set_cap - 1;
    u32 i = slot_of(key, t.set_cap);
    for (; t.set[i] != kEmpty; i = (i + 1) & mask)
        if (t.set[i] == key) return false;
    t.set[i] = key;
    ++t.set_n;
    return true;
}

void set_remove(ChunkTicks& t, u32 key) {
    if (!t.set_n) return;
    const u32 mask = t.set_cap - 1;
    u32 i = slot_of(key, t.set_cap);
    for (; t.set[i] != key; i = (i + 1) & mask)
        if (t.set[i] == kEmpty) return;
    // Close the gap: every entry behind it that is not where it wants to be moves up into it.
    for (u32 j = (i + 1) & mask; t.set[j] != kEmpty; j = (j + 1) & mask) {
        const u32 home = slot_of(t.set[j], t.set_cap);
        if (((j - home) & mask) >= ((j - i) & mask)) {
            t.set[i] = t.set[j];
            i = j;
        }
    }
    t.set[i] = kEmpty;
    --t.set_n;
}

void heap_reserve(ChunkTicks& t, u32 n) {
    if (LIKELY(n <= t.cap)) return;
    u32 cap = t.cap ? t.cap : 8;
    while (cap < n) cap *= 2;
    auto* grown = static_cast<ScheduledTick*>(std::realloc(static_cast<void*>(t.heap), sizeof(ScheduledTick) * cap));
    if (UNLIKELY(!grown)) std::abort();
    t.heap = grown;
    t.cap = cap;
}

void destroy(ChunkTicks* t) {
    if (!t) return;
    std::free(t->heap);
    std::free(t->saved);
    std::free(t->set);
    delete t;
}

}  // namespace

ChunkTicks& LevelTicks::container(Chunk& c) {
    if (UNLIKELY(!c.block_ticks)) c.block_ticks = new ChunkTicks();
    return *c.block_ticks;
}

// The chunk's heap holds a tick (again): it is among the chunks collect() looks at.
void LevelTicks::list(Chunk& c, ChunkTicks& t) {
    if (t.slot != ChunkTicks::kNone) {
        next_[t.slot] = t.heap[0].trigger;  // LevelTicks.updateContainerScheduling
        return;
    }
    t.slot = static_cast<u32>(chunks_.size());
    chunks_.push_back(&c);
    next_.push_back(t.heap[0].trigger);
}

void LevelTicks::unlist(ChunkTicks& t) {
    if (t.slot == ChunkTicks::kNone) return;
    const u32 last = static_cast<u32>(chunks_.size()) - 1;
    if (t.slot != last) {
        chunks_[t.slot] = chunks_[last];
        next_[t.slot] = next_[last];
        chunks_[t.slot]->block_ticks->slot = t.slot;
    }
    chunks_.pop_back();
    next_.pop_back();
    t.slot = ChunkTicks::kNone;
}

// LevelChunkTicks.scheduleUnchecked
void LevelTicks::push(Chunk& c, ChunkTicks& t, const ScheduledTick& tick) {
    heap_reserve(t, t.n + 1);
    t.heap[t.n++] = tick;
    std::push_heap(t.heap, t.heap + t.n, later);
    ++count_;
    list(c, t);
}

// LevelChunkTicks.poll
ScheduledTick LevelTicks::pop(Chunk& c, ChunkTicks& t) {
    c.dirty = true;  // what it was saved with holds a tick that has run
    std::pop_heap(t.heap, t.heap + t.n, later);
    const ScheduledTick tick = t.heap[--t.n];
    set_remove(t, tick.what);
    --count_;
    return tick;
}

void LevelTicks::schedule(Chunk& c, i32 x, i32 y, i32 z, u32 type, i64 trigger, i32 priority) {
    // LevelAccessor.createTick takes the next sub-tick number whether or not the tick is kept.
    const i64 sub = ++sub_tick_;
    if (UNLIKELY(static_cast<u32>(y - min_y_) >= height_ || type >= types_)) return;
    DCHECK(priority >= kTickPriorityMin && priority <= kTickPriorityMax);
    ChunkTicks& t = container(c);
    const u32 what = pack(x, y, z, type);
    if (!set_add(t, what)) return;  // LevelChunkTicks.schedule: one per position and type
    push(c, t, {trigger, order_of(priority, sub), what, 0});
}

bool LevelTicks::has(const Chunk& c, i32 x, i32 y, i32 z, u32 type) const {
    if (!c.block_ticks || static_cast<u32>(y - min_y_) >= height_ || type >= types_) return false;
    return set_has(*c.block_ticks, pack(x, y, z, type));
}

bool LevelTicks::will_tick(i32 x, i32 y, i32 z, u32 type) {
    if (run_head_ == run_.size()) return false;
    const auto hash = [](const Due& d) {
        u64 h = static_cast<u64>(static_cast<u32>(d.x)) * 0x9E3779B97F4A7C15ull;
        h = (h ^ static_cast<u32>(d.z)) * 0x9E3779B97F4A7C15ull;
        h = (h ^ (static_cast<u64>(static_cast<u32>(d.y)) << 32 | d.type)) * 0x9E3779B97F4A7C15ull;
        return static_cast<u32>(h >> 32);
    };
    if (!index_built_) {  // LevelTicks.calculateTickSetIfNeeded
        u32 cap = 16;
        while (cap < 2 * (run_.size() - run_head_)) cap *= 2;
        index_.assign(cap, kEmpty);
        for (usize i = run_head_; i < run_.size(); ++i) {
            u32 at = hash(run_[i]) & (cap - 1);
            while (index_[at] != kEmpty) at = (at + 1) & (cap - 1);
            index_[at] = static_cast<u32>(i);
        }
        index_built_ = true;
    }
    const Due want{x, y, z, type};
    const u32 mask = static_cast<u32>(index_.size()) - 1;
    for (u32 at = hash(want) & mask; index_[at] != kEmpty; at = (at + 1) & mask) {
        const Due& d = run_[index_[at]];
        // What has run already is no longer "to run this tick" (runCollectedTicks removes it).
        if (d.x == x && d.y == y && d.z == z && d.type == type) return index_[at] >= run_head_;
    }
    return false;
}

void LevelTicks::collect(i64 now, u32 max) {
    DCHECK(run_.empty());
    // INTRA_TICK_DRAIN_ORDER between what two chunks have next, as "a comes after b".
    const auto after = [](const Chunk* a, const Chunk* b) {
        return a->block_ticks->heap[0].order > b->block_ticks->heap[0].order;
    };

    // sortContainersToTick: the chunks whose next tick is due, if they tick.
    queue_.clear();
    const u32 n = static_cast<u32>(chunks_.size());
    for (u32 i = 0; i < n; ++i) {
        if (LIKELY(next_[i] > now)) continue;
        Chunk* c = chunks_[i];
        const i64 first = c->block_ticks->heap[0].trigger;
        if (first > now) {
            next_[i] = first;
        } else if (c->block_ticking()) {
            queue_.push_back(c);
        }
    }
    if (queue_.empty()) return;
    std::make_heap(queue_.begin(), queue_.end(), after);

    // drainContainers
    while (run_.size() < max && !queue_.empty()) {
        std::pop_heap(queue_.begin(), queue_.end(), after);
        Chunk* top = queue_.back();
        queue_.pop_back();
        ChunkTicks& t = *top->block_ticks;
        run_.push_back(due_of(*top, pop(*top, t)));
        // drainFromCurrentContainer: on with this chunk for as long as its next tick is due and
        // not behind what the best of the other chunks has next.
        const ScheduledTick* other = queue_.empty() ? nullptr : &queue_.front()->block_ticks->heap[0];
        while (run_.size() < max && t.n && t.heap[0].trigger <= now && !(other && t.heap[0].order > other->order))
            run_.push_back(due_of(*top, pop(*top, t)));
        if (!t.n) {
            unlist(t);
        } else if (t.heap[0].trigger <= now && run_.size() < max) {
            queue_.push_back(top);
            std::push_heap(queue_.begin(), queue_.end(), after);
        } else {
            next_[t.slot] = t.heap[0].trigger;
        }
    }
    // rescheduleLeftoverContainers: what is still queued stays due (next_ says so already).
    queue_.clear();
}

void LevelTicks::unpack(Chunk& c, i64 now) {
    ChunkTicks* t = c.block_ticks;
    if (!t || !t->saved_n) return;
    i64 sub = -static_cast<i64>(t->saved_n);
    heap_reserve(*t, t->n + t->saved_n);
    for (u32 i = 0; i < t->saved_n; ++i) {
        const ScheduledTick& s = t->saved[i];
        ++sub;
        // SavedTick.unpack: its priority is kept (order_of() left it in `order`), its place is new.
        const i32 priority = static_cast<i32>(s.order >> 61) + kTickPriorityMin;
        t->heap[t->n++] = {now + s.trigger, order_of(priority, sub), s.what, 0};
        std::push_heap(t->heap, t->heap + t->n, later);
        ++count_;
    }
    std::free(t->saved);
    t->saved = nullptr;
    t->saved_n = 0;
    list(c, *t);
}

void LevelTicks::release(Chunk& c) {
    ChunkTicks* t = c.block_ticks;
    if (!t) return;
    unlist(*t);
    count_ -= t->n;
    destroy(t);
    c.block_ticks = nullptr;
}

void LevelTicks::write(const ChunkTicks* t, i64 now, base::ByteBuf& out) {
    const u32 total = t ? t->n + t->saved_n : 0;
    if (!total) return;
    u8* p = out.reserve(5 + static_cast<usize>(total) * kRecordBytes);
    *p++ = kVersion;
    store_le<u32>(p, total);
    p += 4;
    const auto put = [&](u32 what, i64 delay, u64 order) {
        store_le<u32>(p, what);
        store_le<i32>(p + 4, static_cast<i32>(std::clamp<i64>(delay, INT32_MIN, INT32_MAX)));
        p[8] = static_cast<u8>(static_cast<i8>(static_cast<i32>(order >> 61) + kTickPriorityMin));
        p += kRecordBytes;
    };
    for (u32 i = 0; i < t->saved_n; ++i) put(t->saved[i].what, t->saved[i].trigger, t->saved[i].order);
    // SUB_TICK_ORDERING: by the order they were scheduled in, whatever their priority.
    std::vector<const ScheduledTick*> sorted(t->n);
    for (u32 i = 0; i < t->n; ++i) sorted[i] = &t->heap[i];
    const u64 sub_mask = (1ull << 61) - 1;
    std::sort(sorted.begin(), sorted.end(),
              [&](const ScheduledTick* a, const ScheduledTick* b) { return (a->order & sub_mask) < (b->order & sub_mask); });
    for (const ScheduledTick* s : sorted) put(s->what, s->trigger - now, s->order);
    out.commit(p);
}

bool ticks_valid(std::span<const u8> bytes, u32 types, u32 height) {
    if (bytes.empty()) return true;
    if (bytes.size() < 5 || bytes[0] != kVersion) return false;
    const u32 count = load_le<u32>(bytes.data() + 1);
    if (bytes.size() != 5 + static_cast<usize>(count) * kRecordBytes) return false;
    for (u32 i = 0; i < count; ++i) {
        const u8* r = bytes.data() + 5 + static_cast<usize>(i) * kRecordBytes;
        const u32 what = load_le<u32>(r);
        const i32 priority = static_cast<i8>(r[8]);
        if ((what >> 17) >= types || ((what >> 8) & 511) >= height || priority < kTickPriorityMin || priority > kTickPriorityMax)
            return false;
    }
    return true;
}

void LevelTicks::read(Chunk& c, std::span<const u8> bytes) {
    DCHECK(ticks_valid(bytes, types_, height_));
    if (bytes.size() <= 5) return;
    const u32 count = load_le<u32>(bytes.data() + 1);
    ChunkTicks& t = container(c);
    DCHECK(!t.saved_n && !t.n);
    t.saved = static_cast<ScheduledTick*>(std::malloc(sizeof(ScheduledTick) * count));
    if (UNLIKELY(!t.saved)) std::abort();
    for (u32 i = 0; i < count; ++i) {
        const u8* r = bytes.data() + 5 + static_cast<usize>(i) * kRecordBytes;
        const u32 what = load_le<u32>(r);
        if (!set_add(t, what)) continue;  // (bytes that name a position twice: the first one counts)
        const i32 delay = load_le<i32>(r + 4);
        t.saved[t.saved_n++] = {delay, order_of(static_cast<i8>(r[8]), 0), what, 0};
    }
}

}  // namespace world
