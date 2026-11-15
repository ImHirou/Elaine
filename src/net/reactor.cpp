#include "net/reactor.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

#include "base/clock.hpp"
#include "base/log.hpp"

namespace net {

namespace {
constexpr u16 kBgid = 0;
constexpr u64 kLingerNs = 5 * base::kNsPerSec;

u32 max_frame_for(Phase p) {
    switch (p) {
        case Phase::Handshake:
        case Phase::Status: return 1024;
        case Phase::Login: return 64 * 1024;
        default: return proto::kMaxFrame;
    }
}
}  // namespace

Reactor::Reactor(const ReactorConfig& cfg, PreSink& sink, u32 index)
    : cfg_(cfg),
      sink_(sink),
      index_(index),
      conns_(cfg.max_conns),
      inbound_(cfg.inbound_ring_bytes),
      recycled_(16384),
      outbox_(16384) {
    free_slots_.reserve(cfg.max_conns);
    for (u32 i = cfg.max_conns; i-- > 0;) {
        conns_[i].slot = i;
        free_slots_.push_back(i);
    }
    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0) std::abort();
}

Reactor::~Reactor() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (wake_fd_ >= 0) ::close(wake_fd_);
    for (Conn& c : conns_) {
        if (c.fd >= 0) ::close(c.fd);
        while (base::ByteBuf* b = c.send_head) {
            c.send_head = b->next;
            delete b;
        }
    }
    // Buffers that were in transit between the threads when the server stopped.
    base::ByteBuf* b;
    while (recycled_.pop(b)) delete b;
    OutMsg m;
    while (outbox_.pop(m)) delete m.buf;
    if (ring_ok_) {
        if (br_) io_uring_free_buf_ring(&ring_, br_, cfg_.recv_bufs, kBgid);
        io_uring_queue_exit(&ring_);
    }
    std::free(recv_mem_);
}

bool Reactor::listen(std::string& err) {
    const bool v6 = cfg_.bind_ip.find(':') != std::string::npos;
    const int fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int one = 1, zero = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);  // one listener per reactor
    int rc;
    if (v6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(cfg_.port);
        if (inet_pton(AF_INET6, cfg_.bind_ip.c_str(), &a.sin6_addr) != 1) {
            err = "bad IPv6 address: " + cfg_.bind_ip;
            ::close(fd);
            return false;
        }
        rc = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(cfg_.port);
        if (inet_pton(AF_INET, cfg_.bind_ip.c_str(), &a.sin_addr) != 1) {
            err = "bad IPv4 address: " + cfg_.bind_ip;
            ::close(fd);
            return false;
        }
        rc = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (rc < 0 || ::listen(fd, 1024) < 0) {
        err = std::string("bind/listen ") + cfg_.bind_ip + ":" + std::to_string(cfg_.port) + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    listen_fd_ = fd;
    return true;
}

void Reactor::stop() {
    stop_.store(true, std::memory_order_release);
    wake_pending_.store(false);
    wake();
}

void Reactor::wake() {
    if (!wake_pending_.exchange(true, std::memory_order_acq_rel)) eventfd_write(wake_fd_, 1);
}

void Reactor::run_on_loop(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(task_mu_);
        tasks_.push_back(std::move(fn));
    }
    tasks_pending_.store(true, std::memory_order_release);
    wake();
}

// ---------------------------------------------------------------------------- loop

io_uring_sqe* Reactor::sqe() {
    io_uring_sqe* s = io_uring_get_sqe(&ring_);
    if (UNLIKELY(!s)) {  // SQ full: push what we have to the kernel and retry
        io_uring_submit(&ring_);
        s = io_uring_get_sqe(&ring_);
        if (!s) std::abort();
    }
    return s;
}

void Reactor::arm_accept() {
    io_uring_sqe* s = sqe();
    io_uring_prep_multishot_accept(s, listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    s->user_data = static_cast<u64>(kAccept) << 56;
    accept_armed_ = true;
}

void Reactor::arm_recv(Conn& c) {
    io_uring_sqe* s = sqe();
    io_uring_prep_recv_multishot(s, c.fd, nullptr, 0, 0);
    s->flags |= IOSQE_BUFFER_SELECT;
    s->buf_group = kBgid;
    s->user_data = tag(kRecv, c);
    c.recv_armed = true;
}

void Reactor::arm_wake() {
    io_uring_sqe* s = sqe();
    io_uring_prep_read(s, wake_fd_, &wake_val_, sizeof wake_val_, 0);
    s->user_data = static_cast<u64>(kWake) << 56;
}

void Reactor::return_buf(u32 bid) {
    io_uring_buf_ring_add(br_, recv_mem_ + static_cast<usize>(bid) * cfg_.recv_buf_size, cfg_.recv_buf_size,
                          static_cast<u16>(bid), io_uring_buf_ring_mask(cfg_.recv_bufs), static_cast<int>(br_pending_));
    ++br_pending_;
}

void Reactor::run() {
    // The ring is created on the thread that uses it: SINGLE_ISSUER + DEFER_TASKRUN make the
    // kernel run completion work only when this thread asks for it (no IPIs, no task_work storms).
    io_uring_params params{};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    params.cq_entries = 16384;
    int rc = io_uring_queue_init_params(4096, &ring_, &params);
    if (rc < 0) {  // older kernel: fall back to a plain ring
        params = {};
        rc = io_uring_queue_init_params(4096, &ring_, &params);
    }
    if (rc < 0) {
        LOG_ERROR("io_uring_queue_init failed: %s", std::strerror(-rc));
        std::abort();
    }
    ring_ok_ = true;
    io_uring_register_ring_fd(&ring_);

    DCHECK(std::has_single_bit(cfg_.recv_bufs));
    int err = 0;
    br_ = io_uring_setup_buf_ring(&ring_, cfg_.recv_bufs, kBgid, 0, &err);
    if (!br_) {
        LOG_ERROR("io_uring_setup_buf_ring failed: %s (kernel >= 6.0 required)", std::strerror(-err));
        std::abort();
    }
    const usize recv_bytes = (static_cast<usize>(cfg_.recv_bufs) * cfg_.recv_buf_size + 4095) & ~usize{4095};
    recv_mem_ = static_cast<u8*>(std::aligned_alloc(4096, recv_bytes));
    if (!recv_mem_) std::abort();
    for (u32 i = 0; i < cfg_.recv_bufs; ++i) return_buf(i);
    io_uring_buf_ring_advance(br_, static_cast<int>(br_pending_));
    br_pending_ = 0;

    arm_accept();
    arm_wake();
    u64 next_house = base::now_ns() + base::kNsPerSec;

    while (!stop_.load(std::memory_order_acquire)) {
        __kernel_timespec ts{0, 250 * 1'000'000};
        io_uring_cqe* cqe = nullptr;
        io_uring_submit_and_wait_timeout(&ring_, &cqe, 1, &ts, nullptr);

        unsigned head;
        unsigned seen = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++seen;
            const u64 ud = cqe->user_data;
            const Op op = static_cast<Op>(ud >> 56);
            if (op == kAccept) {
                on_accept(cqe->res, cqe->flags);
                continue;
            }
            if (op == kWake) {
                wake_pending_.store(false, std::memory_order_release);
                arm_wake();
                continue;
            }
            Conn& c = conns_[static_cast<u32>(ud)];
            const bool live = c.open && (c.gen & 0xFFFFFF) == ((ud >> 32) & 0xFFFFFF);
            if (op == kRecv) {
                if (LIKELY(live))
                    on_recv(c, cqe);
                else if (cqe->flags & IORING_CQE_F_BUFFER)
                    return_buf(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            } else if (op == kSend && live) {
                on_send(c, cqe->res);
            }
        }
        io_uring_cq_advance(&ring_, seen);

        inbound_.publish();  // everything parsed in this batch becomes visible to the tick thread at once
        if (br_pending_) {
            io_uring_buf_ring_advance(br_, static_cast<int>(br_pending_));
            br_pending_ = 0;
        }
        for (u32 slot : rearm_) {
            Conn& c = conns_[slot];
            if (c.open && !c.closing && !c.recv_armed && c.fd >= 0) arm_recv(c);
        }
        rearm_.clear();
        if (!accept_armed_) arm_accept();

        if (tasks_pending_.load(std::memory_order_acquire)) drain_tasks();
        drain_outbox();

        for (usize i = 0; i < dirty_.size(); ++i) {  // sinks may dirty more connections while we flush
            Conn& c = conns_[dirty_[i]];
            flush_staged(c);
            if (c.open && c.close_after_send && !c.closing && !c.send_busy && !c.send_head) begin_linger(c);
        }
        dirty_.clear();

        if (!pending_leave_.empty()) {
            usize kept = 0;
            for (u32 slot : pending_leave_)
                if (!push_leave(conns_[slot])) pending_leave_[kept++] = slot;
            pending_leave_.resize(kept);
            inbound_.publish();
        }

        const u64 now = base::now_ns();
        if (now >= next_house) {
            housekeeping(now);
            next_house = now + base::kNsPerSec;
        }
    }
    // Shutting down: let the sink free the state of connections that never became players.
    for (Conn& c : conns_)
        if (c.open && c.phase != Phase::Play) sink_.on_close(*this, c);
}

void Reactor::housekeeping(u64 now) {
    for (Conn& c : conns_) {
        if (!c.open || c.closing || c.zombie) continue;
        if ((c.phase != Phase::Play || c.close_after_send) && now > c.deadline_ns) close(c);
    }
}

void Reactor::drain_tasks() {
    std::vector<std::function<void()>> run;
    {
        std::lock_guard<std::mutex> lk(task_mu_);
        run.swap(tasks_);
        tasks_pending_.store(false, std::memory_order_release);
    }
    for (auto& fn : run) fn();
}

void Reactor::drain_outbox() {
    OutMsg m;
    while (outbox_.pop(m)) {
        Conn* c = m.slot < cfg_.max_conns ? &conns_[m.slot] : nullptr;
        const bool valid = c && c->open && c->gen == m.gen;
        switch (m.kind) {
            case OutKind::Send:
                if (valid && !c->closing && !c->close_after_send)
                    queue_send(*c, m.buf);
                else
                    recycle(m.buf);
                break;
            case OutKind::SendClose:
                if (valid && !c->closing && !c->close_after_send) {
                    if (m.buf) queue_send(*c, m.buf);
                    c->close_after_send = true;
                    c->deadline_ns = base::now_ns() + kLingerNs;
                    if (!c->send_busy && !c->send_head) begin_linger(*c);
                } else if (m.buf) {
                    recycle(m.buf);
                }
                break;
            case OutKind::Release:
                if (valid && c->zombie) free_slot(*c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------- accept / recv

void Reactor::on_accept(int res, u32 flags) {
    if (!(flags & IORING_CQE_F_MORE)) accept_armed_ = false;
    if (res < 0) {
        if (res != -EAGAIN && res != -ECONNABORTED) LOG_WARN("accept: %s", std::strerror(-res));
        return;
    }
    const int fd = res;
    if (free_slots_.empty()) {
        ::close(fd);
        return;
    }
    Conn& c = conns_[free_slots_.back()];
    free_slots_.pop_back();
    c.fd = fd;
    c.open = true;
    c.deadline_ns = base::now_ns() + cfg_.prelogin_timeout_ns;
    set_phase(c, Phase::Handshake);

    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // we batch per tick ourselves
    sockaddr_storage ss{};
    socklen_t sl = sizeof ss;
    char ip[INET6_ADDRSTRLEN] = "?";
    u16 port = 0;
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &sl) == 0) {
        if (ss.ss_family == AF_INET) {
            auto* a = reinterpret_cast<sockaddr_in*>(&ss);
            inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
            port = ntohs(a->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
            inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
            port = ntohs(a->sin6_port);
        }
    }
    c.remote = std::string(ip) + ":" + std::to_string(port);
    sink_.on_open(*this, c);
    arm_recv(c);
}

void Reactor::on_recv(Conn& c, io_uring_cqe* cqe) {
    const bool has_buf = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
    const u32 bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    if (!(cqe->flags & IORING_CQE_F_MORE)) c.recv_armed = false;
    const int res = cqe->res;

    if (res > 0 && has_buf && !c.closing && !c.close_after_send)
        process(c, recv_mem_ + static_cast<usize>(bid) * cfg_.recv_buf_size, static_cast<u32>(res));
    if (has_buf) return_buf(bid);

    if (res <= 0) {
        if (res == -ENOBUFS && !c.closing) {  // provided buffers ran dry; re-arm once they are returned
            rearm_.push_back(c.slot);
            return;
        }
        close(c);  // EOF or error
        return;
    }
    if (c.closing)
        finish_close(c);
    else if (!c.recv_armed)
        rearm_.push_back(c.slot);
}

HOT void Reactor::process(Conn& c, u8* data, u32 n) {
    if (c.encrypted) c.dec.decrypt(data, n);

    // Fast path: no partial frame pending -> parse straight out of the kernel-provided buffer.
    const bool direct = c.in.empty();
    const u8* p;
    const u8* end;
    if (LIKELY(direct)) {
        p = data;
        end = data + n;
    } else {
        c.in.append(data, n);
        p = c.in.data + c.in.rd;
        end = c.in.data + c.in.len;
    }

    for (;;) {
        std::span<const u8> frame;
        u32 used = 0;
        const proto::FrameStatus st = proto::frame_split(p, end, c.max_frame, frame, used);
        if (st == proto::FrameStatus::NeedMore) break;
        if (UNLIKELY(st == proto::FrameStatus::Bad)) {
            LOG_DEBUG("%s: bad frame", c.remote.c_str());
            close(c);
            return;
        }
        std::span<const u8> body = frame;
        if (c.threshold >= 0 && UNLIKELY(!proto::frame_inflate(frame, c.threshold, body))) {
            LOG_DEBUG("%s: bad compressed frame", c.remote.c_str());
            close(c);
            return;
        }
        p += used;

        if (LIKELY(c.phase == Phase::Play)) {
            u8* rec = inbound_.alloc(static_cast<u32>(sizeof(InHeader) + body.size()));
            if (UNLIKELY(!rec)) {  // tick thread is not draining: shed this connection rather than block the loop
                LOG_WARN("%s: inbound ring full, dropping connection", c.remote.c_str());
                close(c);
                return;
            }
            const InHeader h{c.slot, c.gen, InKind::Packet, 0};
            std::memcpy(rec, &h, sizeof h);
            std::memcpy(rec + sizeof h, body.data(), body.size());
            inbound_.commit();
        } else {
            sink_.on_frame(*this, c, body);
            if (c.closing || c.close_after_send) return;
            if (UNLIKELY(c.crypt_switched)) {  // rest of this buffer arrived under the new cipher
                c.crypt_switched = false;
                c.dec.decrypt(const_cast<u8*>(p), static_cast<usize>(end - p));
            }
        }
    }

    if (direct) {
        if (p != end) c.in.append(p, static_cast<usize>(end - p));
    } else {
        c.in.rd = static_cast<u32>(p - c.in.data);
        if (c.in.rd == c.in.len)
            c.in.clear();
        else if (c.in.rd >= 4096)
            c.in.compact();
    }
}

// ---------------------------------------------------------------------------- send

void Reactor::flush_staged(Conn& c) {
    c.dirty = false;
    if (!c.open || c.closing || c.fd < 0 || c.out.len == 0) {
        c.out.clear();
        return;
    }
    // Steal the staged storage instead of copying it; the connection gets a pooled buffer back.
    base::ByteBuf* b = pool_.get(1024);
    std::swap(b->data, c.out.data);
    std::swap(b->cap, c.out.cap);
    b->len = c.out.len;
    b->rd = 0;
    b->tag = kBufOwnerNet;
    c.out.len = 0;
    if (c.encrypted) c.enc.encrypt(b->data, b->len);
    queue_send(c, b);
}

void Reactor::queue_send(Conn& c, base::ByteBuf* b) {
    b->next = nullptr;
    b->rd = 0;
    if (c.send_tail)
        c.send_tail->next = b;
    else
        c.send_head = b;
    c.send_tail = b;
    if (!c.send_busy) start_send(c);
}

void Reactor::start_send(Conn& c) {
    base::ByteBuf* b = c.send_head;
    io_uring_sqe* s = sqe();
    // MSG_WAITALL: the kernel keeps going on short writes, so one SQE normally covers the buffer.
    io_uring_prep_send(s, c.fd, b->data + b->rd, b->len - b->rd, MSG_NOSIGNAL | MSG_WAITALL);
    s->user_data = tag(kSend, c);
    c.send_busy = true;
}

void Reactor::on_send(Conn& c, int res) {
    c.send_busy = false;
    if (c.closing) {
        finish_close(c);
        return;
    }
    if (res <= 0) {
        close(c);
        return;
    }
    base::ByteBuf* b = c.send_head;
    b->rd += static_cast<u32>(res);
    if (b->rd >= b->len) {
        c.send_head = b->next;
        if (!c.send_head) c.send_tail = nullptr;
        recycle(b);
    }
    if (c.send_head)
        start_send(c);
    else if (c.close_after_send && !c.dirty)
        begin_linger(c);
}

void Reactor::recycle(base::ByteBuf* b) {
    if (!b) return;
    b->next = nullptr;
    if (b->tag == kBufOwnerTick) {
        if (!recycled_.push(b)) delete b;
    } else {
        pool_.put(b);
    }
}

// ---------------------------------------------------------------------------- state changes / close

void Reactor::enable_encryption(Conn& c, const u8 key[16]) {
    flush_staged(c);  // anything staged so far belongs to the plaintext part of the stream
    c.enc.init(key);
    c.dec.init(key);
    c.encrypted = true;
    c.crypt_switched = true;
}

void Reactor::set_phase(Conn& c, Phase p) {
    c.phase = p;
    c.max_frame = max_frame_for(p);
}

bool Reactor::promote_to_play(Conn& c, std::span<const u8> blob) {
    flush_staged(c);
    u8* rec = inbound_.alloc(static_cast<u32>(sizeof(InHeader) + sizeof(JoinTransport) + blob.size()));
    if (!rec) return false;
    const InHeader h{c.slot, c.gen, InKind::Join, 0};
    JoinTransport t;
    t.enc = c.enc;
    t.threshold = c.threshold;
    t.encrypted = c.encrypted ? 1 : 0;
    std::memcpy(rec, &h, sizeof h);
    std::memcpy(rec + sizeof h, &t, sizeof t);
    std::memcpy(rec + sizeof h + sizeof t, blob.data(), blob.size());
    inbound_.commit();
    set_phase(c, Phase::Play);
    c.pre = nullptr;
    return true;
}

void Reactor::close_after_flush(Conn& c) {
    if (c.closing || c.close_after_send) return;
    c.close_after_send = true;
    c.deadline_ns = base::now_ns() + kLingerNs;
    mark_dirty(c);
}

// Half-close: our FIN goes out behind the queued data and we wait for the peer to close. A
// full close with unread input would turn into a RST and could eat the disconnect message.
void Reactor::begin_linger(Conn& c) {
    if (c.lingering || c.closing) return;
    c.lingering = true;
    ::shutdown(c.fd, SHUT_WR);
}

void Reactor::close(Conn& c) {
    if (!c.open || c.fd < 0) return;
    if (!c.closing) {
        c.closing = true;
        ::shutdown(c.fd, SHUT_RDWR);  // makes the armed recv / in-flight send complete
    }
    finish_close(c);
}

void Reactor::finish_close(Conn& c) {
    if (!c.closing || c.fd < 0 || c.recv_armed || c.send_busy) return;
    ::close(c.fd);
    c.fd = -1;
    while (base::ByteBuf* b = c.send_head) {
        c.send_head = b->next;
        recycle(b);
    }
    c.send_tail = nullptr;
    if (c.phase == Phase::Play) {
        c.zombie = true;  // the tick thread still references this slot; wait for its Release
        if (!push_leave(c)) pending_leave_.push_back(c.slot);
    } else {
        sink_.on_close(*this, c);
        free_slot(c);
    }
}

bool Reactor::push_leave(const Conn& c) {
    u8* rec = inbound_.alloc(sizeof(InHeader));
    if (!rec) return false;
    const InHeader h{c.slot, c.gen, InKind::Leave, 0};
    std::memcpy(rec, &h, sizeof h);
    inbound_.commit();
    return true;
}

void Reactor::free_slot(Conn& c) {
    c.open = false;
    ++c.gen;
    c.phase = Phase::Handshake;
    c.encrypted = c.recv_armed = c.send_busy = c.closing = c.close_after_send = false;
    c.lingering = c.zombie = c.dirty = c.crypt_switched = false;
    c.threshold = proto::kNoCompression;
    c.pre = nullptr;
    c.remote.clear();
    if (c.in.cap > (64u << 10)) c.in.reset();
    c.in.clear();
    if (c.out.cap > (256u << 10)) c.out.reset();
    c.out.clear();
    free_slots_.push_back(c.slot);
}

}  // namespace net
