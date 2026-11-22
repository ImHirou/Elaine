// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/store.hpp"

#include <cstdlib>
#include <cstring>

namespace entity {

namespace {
constexpr u32 kGenMask = (1u << 29) - 1;

template <class T>
void regrow(T*& p, u32 used, u32 cap) {
    // 64-byte alignment and a capacity that is a multiple of 64 rows: the tracker's SIMD scans
    // read whole 64-row blocks, and rows past the end must read as zeros, not as garbage.
    // Rows are moved to the new block as bytes: every column type, item stacks included, can be
    // relocated that way, and all-zero bytes are its empty value.
    const usize bytes = (static_cast<usize>(cap) * sizeof(T) + 63) & ~usize{63};
    T* fresh = static_cast<T*>(std::aligned_alloc(64, bytes));
    if (UNLIKELY(!fresh)) std::abort();
    if (used) std::memcpy(static_cast<void*>(fresh), p, static_cast<usize>(used) * sizeof(T));
    std::memset(static_cast<void*>(fresh + used), 0, bytes - static_cast<usize>(used) * sizeof(T));
    std::free(p);
    p = fresh;
}
}  // namespace

Store::Store() { grow(1024); }

Store::~Store() {
    for (u32 i = 0; i < n_; ++i) item[i] = {};  // the one column whose values own something (a patch reference)
    each_column([](auto*& col) { std::free(col); });
}

void Store::grow(u32 cap) {
    cap = (cap + 63u) & ~63u;
    each_column([&](auto*& col) { regrow(col, n_, cap); });
    cap_ = cap;
}

void Store::clear_row(u32 i) {
    each_column([&](auto*& col) { col[i] = {}; });
}

void Store::move_row(u32 from, u32 to, Moves& moved) {
    each_column([&](auto*& col) { col[to] = col[from]; });
    slot_dense_[slot[to]] = to;
    moved.m[moved.n++] = {from, to};
}

u32 Store::create(u32 type_id, bool player, Uuid id, double px, double py, double pz, float yaw_deg, float pitch_deg,
                  Moves& moved) {
    DCHECK(type_id < proto::gen::entity_type::kCount && n_ < kMaxEntities);
    moved.n = 0;
    if (UNLIKELY(n_ == cap_)) grow(cap_ * 2);
    u32 ei = n_;
    if (player) {
        ei = players_++;
        if (ei != n_) move_row(ei, n_, moved);  // the first non-player gives up its row
    }
    ++n_;
    clear_row(ei);

    u32 s;
    if (!free_slots_.empty()) {
        s = free_slots_.back();
        free_slots_.pop_back();
    } else {
        s = static_cast<u32>(slot_dense_.size());
        slot_dense_.push_back(kNone);
        slot_gen_.push_back(1);  // never 0: a zero handle is nobody
    }
    slot_dense_[s] = ei;

    const proto::gen::entity_type::Info& info = proto::gen::entity_type::kInfo[type_id];
    x[ei] = px;
    y[ei] = py;
    z[ei] = pz;
    yaw[ei] = yaw_deg;
    pitch[ei] = pitch_deg;
    head_yaw[ei] = info.living ? yaw_deg : 0.0f;  // Entity.getYHeadRot() is 0 for everything not alive
    flags[ei] = static_cast<u16>(flag::kMoved | (info.living ? flag::kLiving : 0) | (info.track_deltas ? flag::kTrackDeltas : 0));
    type[ei] = static_cast<u16>(type_id);
    slot[ei] = s;
    main_hand[ei] = kMainHandRight;
    uuid[ei] = id;
    return ei;
}

void Store::destroy(u32 ei, Moves& moved) {
    DCHECK(ei < n_);
    moved.n = 0;
    const u32 s = slot[ei];
    slot_dense_[s] = kNone;
    if (s < cold_of_slot_.size() && cold_of_slot_[s]) {  // what a tag said of it goes with it
        cold_[cold_of_slot_[s] - 1] = {};
        cold_free_.push_back(cold_of_slot_[s]);
        cold_of_slot_[s] = 0;
    }
    slot_gen_[s] = (slot_gen_[s] + 1) & kGenMask;
    if (slot_gen_[s] == 0) slot_gen_[s] = 1;
    free_slots_.push_back(s);

    u32 hole = ei;
    if (ei < players_) {  // keep the player prefix dense: the last player fills the hole first
        const u32 last_player = --players_;
        if (hole != last_player) move_row(last_player, hole, moved);
        hole = last_player;
    }
    const u32 last = --n_;
    if (hole != last) move_row(last, hole, moved);
    clear_row(last);
}

Cold& Store::make_cold(u32 ei) {
    const u32 s = slot[ei];
    if (s >= cold_of_slot_.size()) cold_of_slot_.resize(s + 1, 0);
    if (!cold_of_slot_[s]) {
        if (!cold_free_.empty()) {
            cold_of_slot_[s] = cold_free_.back();
            cold_free_.pop_back();
        } else {
            cold_.emplace_back();
            cold_of_slot_[s] = static_cast<u32>(cold_.size());
        }
    }
    return cold_[cold_of_slot_[s] - 1];
}

}  // namespace entity
