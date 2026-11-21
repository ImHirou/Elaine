// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/store.hpp"

#include <cstdio>
#include <filesystem>

#include "base/log.hpp"

namespace world {

namespace {

constexpr u64 kWriteDelay = 100;     // ticks a chunk's bytes wait for others of its region: 5 s
constexpr usize kWriteAtOnce = 256;  // ...unless this many are waiting
constexpr u64 kCloseAfter = 1200;    // ticks a region stays open with nobody using it: a minute
constexpr u64 kRetryAfter = 200;     // ticks until a write that failed is tried again

u64 key_of(i32 rx, i32 rz) { return (static_cast<u64>(static_cast<u32>(rx)) << 32) | static_cast<u32>(rz); }
u16 local_of(i32 cx, i32 cz) { return static_cast<u16>((static_cast<u32>(cz) & 31) << 5 | (static_cast<u32>(cx) & 31)); }

}  // namespace

struct ChunkStore::LoadJob {
    ChunkStore* store;
    Region* region;  // whose file is read, null when none is
    i32 cx, cz;
    int fd;
    RegionEntry entry;
    Blob* blob;
    u64 seq;
    LoadedChunk* out = nullptr;
    std::string error;
};
struct ChunkStore::OpenJob {
    ChunkStore* store;
    Region* region;
    std::string path;
    bool create;
    RegionFile* file = nullptr;
    bool ok = false;
    std::string error;
};
struct ChunkStore::WriteJob {
    ChunkStore* store;
    Region* region;
    RegionFile* file;  // the job's while it runs
    std::vector<Item> items;
    bool sync;
    bool ok = false;
    std::string error;
};

ChunkStore::ChunkStore(base::Workers& workers, const Layout& layout, ChunkGenerator& gen, std::string dir, bool sync)
    : workers_(workers), layout_(layout), gen_(gen), dir_(std::move(dir)), sync_(sync) {
    if (dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        LOG_ERROR("cannot make %s: %s - the world will not be saved", dir_.c_str(), ec.message().c_str());
        failed_ = true;
    }
    // The regions that exist: asked of the directory once, so that a chunk of a region that has no
    // file is known to be the generator's without touching the disk.
    for (std::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        int rx = 0, rz = 0, used = 0;
        if (std::sscanf(name.c_str(), "r.%d.%d.rgn%n", &rx, &rz, &used) == 2 && static_cast<usize>(used) == name.size())
            region(rx, rz).state = Region::kClosed;
    }
}

void ChunkStore::finish() {
    if (!persistent()) return workers_.drain();
    flush_ = true;
    u64 before = written_;
    for (u32 idle = 0;;) {
        tick(now_);
        workers_.drain();
        if (waiting_ == 0 && jobs_ == 0) break;
        // A round may only open a region; three that write nothing mean that what is left waits
        // on a region that cannot be written.
        idle = written_ == before ? idle + 1 : 0;
        before = written_;
        if (idle >= 3) {
            LOG_ERROR("%zu chunk(s) could not be saved", waiting_);
            break;
        }
        now_ += kRetryAfter;  // (nobody is counting ticks any more)
    }
    flush_ = false;
}

ChunkStore::~ChunkStore() {
    // What is still out with a worker points into this object: have it back first.
    workers_.drain();
    for (LoadedChunk* lc : loaded_) delete lc;
    for (const Arrived& a : arrived_) delete a.chunk;
    for (auto& [key, r] : regions_) {
        for (const Item& it : r->pending) std::free(it.blob);
        for (const Item& it : r->in_flight) std::free(it.blob);
        delete r->file;
        delete r;
    }
}

ChunkStore::Region& ChunkStore::region(i32 rx, i32 rz) {
    Region*& r = regions_[key_of(rx, rz)];
    if (!r) {
        r = new Region();
        r->rx = rx;
        r->rz = rz;
    }
    return *r;
}

std::string ChunkStore::path_of(const Region& r) const {
    return dir_ + "/r." + std::to_string(r.rx) + "." + std::to_string(r.rz) + ".rgn";
}

ChunkStore::Blob* ChunkStore::find(std::vector<Item>& items, u16 index) {
    for (Item& it : items)
        if (it.index == index) return it.blob;
    return nullptr;
}

void ChunkStore::release(Blob* b) {
    if (--b->refs == 0) std::free(b);
}

// ---------------------------------------------------------------------------- loading

void ChunkStore::load(i32 cx, i32 cz) {
    const u64 seq = asked_++;
    if (!persistent()) return start_load(nullptr, cx, cz, {}, nullptr, seq);
    Region& r = region(cx >> 5, cz >> 5);
    r.last_used = now_;
    const u16 index = local_of(cx, cz);
    // The newest bytes first: what waits to be written, then what is being written.
    Blob* blob = find(r.pending, index);
    if (!blob) blob = find(r.in_flight, index);
    if (blob) return start_load(nullptr, cx, cz, {}, blob, seq);
    switch (r.state) {
        case Region::kNone:
        case Region::kFailed: return start_load(nullptr, cx, cz, {}, nullptr, seq);
        case Region::kClosed: r.waiting.push_back({index, seq}); return start_open(r, false);
        case Region::kOpening: r.waiting.push_back({index, seq}); return;
        case Region::kOpen: return load_known(r, index, seq);
    }
}

void ChunkStore::load_known(Region& r, u16 index, u64 seq) {
    const i32 cx = r.rx * 32 + (index & 31), cz = r.rz * 32 + (index >> 5);
    const RegionEntry& e = r.index[index];
    start_load(e.sector ? &r : nullptr, cx, cz, e, nullptr, seq);
}

void ChunkStore::start_load(Region* r, i32 cx, i32 cz, const RegionEntry& entry, Blob* blob, u64 seq) {
    if (r) ++r->readers;
    if (blob) ++blob->refs;
    auto* job = new LoadJob{this, r, cx, cz, r ? r->file->fd : -1, entry, blob, seq, nullptr, {}};
    ++jobs_;
    workers_.submit({&load_work, &load_done, this, job, 0, 0, 0, 0}, 2);
}

void ChunkStore::load_work(base::Task& t) {
    auto* job = static_cast<LoadJob*>(t.data);
    ChunkStore& s = *job->store;
    thread_local std::vector<u8> raw;
    std::span<const u8> bytes;
    if (job->blob) {
        bytes = {job->blob->data(), job->blob->len};
    } else if (job->region) {
        if (region_read(job->fd, job->entry, raw, job->error)) bytes = raw;
    }
    auto* lc = new LoadedChunk();
    if (job->error.empty() && load_chunk(bytes, job->cx, job->cz, s.layout_, s.gen_, *lc, job->error)) {
        job->out = lc;
        return;
    }
    // What is on disk for it cannot be read. The world goes on with what the generator makes there;
    // the bytes stay where they are until the chunk is saved again.
    delete lc;
    lc = new LoadedChunk();
    std::string ignored;
    load_chunk({}, job->cx, job->cz, s.layout_, s.gen_, *lc, ignored);
    job->out = lc;
}

void ChunkStore::load_done(base::Task& t) {
    auto* job = static_cast<LoadJob*>(t.data);
    ChunkStore& s = *job->store;
    --s.jobs_;
    if (job->region) --job->region->readers;
    if (job->blob) s.release(job->blob);
    if (!job->error.empty()) LOG_ERROR("chunk %d, %d cannot be read (%s): generated anew", job->cx, job->cz, job->error.c_str());
    // Handed on in the order of asking: this one, if it is next, and whatever was waiting behind it.
    s.arrived_.push_back({job->seq, job->out});
    for (bool found = true; found;) {
        found = false;
        for (usize i = 0; i < s.arrived_.size(); ++i) {
            if (s.arrived_[i].seq != s.handed_) continue;
            s.loaded_.push_back(s.arrived_[i].chunk);
            s.arrived_[i] = s.arrived_.back();
            s.arrived_.pop_back();
            ++s.handed_;
            found = true;
            break;
        }
    }
    delete job;
}

// ---------------------------------------------------------------------------- regions

void ChunkStore::start_open(Region& r, bool create) {
    r.state = Region::kOpening;
    r.want_create = create;
    auto* job = new OpenJob{this, &r, path_of(r), create, nullptr, false, {}};
    ++jobs_;
    workers_.submit({&open_work, &open_done, this, job, 0, 0, 0, 0}, 1);
}

void ChunkStore::open_work(base::Task& t) {
    auto* job = static_cast<OpenJob*>(t.data);
    job->file = new RegionFile();
    job->ok = job->file->open(job->path, job->create, job->error);
}

void ChunkStore::open_done(base::Task& t) {
    auto* job = static_cast<OpenJob*>(t.data);
    ChunkStore& s = *job->store;
    Region& r = *job->region;
    --s.jobs_;
    if (job->ok) {
        r.state = Region::kOpen;
        r.file = job->file;
        std::memcpy(r.index, r.file->table, sizeof r.index);
        r.last_used = s.now_;
    } else {
        delete job->file;
        if (job->error.empty()) {
            r.state = Region::kNone;  // the file is gone: its chunks are the generator's
        } else {
            // A region that cannot be read is not written either: whatever is in it stays as it is.
            r.state = Region::kFailed;
            s.failed_ = true;
            LOG_ERROR("%s - its chunks are generated anew and will not be saved", job->error.c_str());
        }
    }
    std::vector<Region::Waiting> waiting;
    waiting.swap(r.waiting);
    for (const Region::Waiting& w : waiting) {
        if (r.state == Region::kOpen)
            s.load_known(r, w.index, w.seq);
        else
            s.start_load(nullptr, r.rx * 32 + (w.index & 31), r.rz * 32 + (w.index >> 5), {}, nullptr, w.seq);
    }
    delete job;
}

// ---------------------------------------------------------------------------- saving

void ChunkStore::save(i32 cx, i32 cz, std::span<const u8> bytes) {
    DCHECK(persistent());
    Region& r = region(cx >> 5, cz >> 5);
    r.last_used = now_;
    auto* blob = static_cast<Blob*>(std::malloc(sizeof(Blob) + bytes.size()));
    if (UNLIKELY(!blob)) std::abort();
    blob->refs = 1;
    blob->len = static_cast<u32>(bytes.size());
    std::memcpy(blob->data(), bytes.data(), bytes.size());
    const u16 index = local_of(cx, cz);
    for (Item& it : r.pending) {
        if (it.index != index) continue;
        release(it.blob);
        it.blob = blob;
        return;
    }
    if (r.pending.empty()) r.first_pending = now_;
    r.pending.push_back({index, blob});
    ++waiting_;
    if (!r.listed) {
        r.listed = true;
        dirty_.push_back(&r);
    }
}

void ChunkStore::start_write(Region& r) {
    r.writing = true;
    auto* job = new WriteJob{this, &r, r.file, {}, sync_, false, {}};
    job->items.swap(r.pending);
    waiting_ -= job->items.size();
    // The loads that come while it is written are served from these bytes.
    for (const Item& it : job->items) {
        if (Blob* older = find(r.in_flight, it.index)) {
            std::erase_if(r.in_flight, [&](const Item& o) { return o.blob == older; });
            release(older);
        }
        ++it.blob->refs;
        r.in_flight.push_back(it);
    }
    ++jobs_;
    workers_.submit({&write_work, &write_done, this, job, 0, 0, 0, 0}, static_cast<u32>(4 * job->items.size()));
}

void ChunkStore::write_work(base::Task& t) {
    auto* job = static_cast<WriteJob*>(t.data);
    std::vector<RegionFile::Put> batch;
    batch.reserve(job->items.size());
    for (const Item& it : job->items) batch.push_back({it.index, {it.blob->data(), it.blob->len}});
    job->ok = job->file->write(batch, job->sync, job->error);
}

void ChunkStore::write_done(base::Task& t) {
    auto* job = static_cast<WriteJob*>(t.data);
    ChunkStore& s = *job->store;
    Region& r = *job->region;
    --s.jobs_;
    r.writing = false;
    r.last_used = s.now_;
    if (job->ok) {
        std::memcpy(r.index, r.file->table, sizeof r.index);
        s.written_ += job->items.size();
        for (const Item& it : job->items) {
            // Out of in_flight - unless a later write of the same chunk has taken its place there.
            std::erase_if(r.in_flight, [&](const Item& o) {
                if (o.blob != it.blob) return false;
                s.release(o.blob);
                return true;
            });
            s.release(it.blob);
        }
    } else {
        // Not written: the bytes wait again, behind whatever newer ones came in the meantime.
        LOG_ERROR("%s: %s - %zu chunk(s) wait to be written again", s.path_of(r).c_str(), job->error.c_str(), job->items.size());
        s.failed_ = true;
        r.retry_at = s.now_ + kRetryAfter;
        for (const Item& it : job->items) {
            std::erase_if(r.in_flight, [&](const Item& o) {
                if (o.blob != it.blob) return false;
                s.release(o.blob);
                return true;
            });
            if (find(r.pending, it.index)) {
                s.release(it.blob);
                continue;
            }
            if (r.pending.empty()) r.first_pending = s.now_;
            r.pending.push_back(it);
            ++s.waiting_;
        }
        if (!r.pending.empty() && !r.listed) {
            r.listed = true;
            s.dirty_.push_back(&r);
        }
    }
    delete job;
}

void ChunkStore::tick(u64 now) {
    now_ = now;
    for (usize i = 0; i < dirty_.size();) {
        Region& r = *dirty_[i];
        if (r.pending.empty()) {  // written, or taken by a write that is under way
            r.listed = false;
            dirty_[i] = dirty_.back();
            dirty_.pop_back();
            continue;
        }
        ++i;
        if (r.writing || now < r.retry_at) continue;
        if (!flush_ && now - r.first_pending < kWriteDelay && r.pending.size() < kWriteAtOnce) continue;
        switch (r.state) {
            case Region::kNone:
            case Region::kClosed: start_open(r, true); break;
            case Region::kOpening: break;
            case Region::kOpen: start_write(r); break;
            case Region::kFailed:
                LOG_ERROR("%s: %zu chunk(s) are not saved", path_of(r).c_str(), r.pending.size());
                for (const Item& it : r.pending) release(it.blob);
                waiting_ -= r.pending.size();
                r.pending.clear();
                break;
        }
    }
    if (flush_ && waiting_ == 0) flush_ = false;
    if (now % 200 == 0) {
        for (auto& [key, r] : regions_) {
            if (r->state != Region::kOpen || r->readers || r->writing || !r->pending.empty() || !r->in_flight.empty()) continue;
            if (now - r->last_used < kCloseAfter) continue;
            delete r->file;
            r->file = nullptr;
            r->state = Region::kClosed;
        }
    }
}

}  // namespace world
