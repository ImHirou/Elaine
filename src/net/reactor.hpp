// Reactor: one io_uring event loop on one thread. Owns sockets, inbound decryption, frame
// splitting/inflating and the send queues. Everything protocol-specific before the play state
// is delegated to a PreSink on this same thread; play-state packets are copied into an SPSC
// byte ring for the tick thread and never touch a lock.
//
// Thread rules (the whole design hangs on these):
//   * Conn and every method marked [loop] belong to the reactor thread only.
//   * The tick thread talks to a reactor only through inbound() / recycled() / post() / wake().
//   * Before promote_to_play() the reactor thread writes a connection's outbound stream; after
//     it, only the tick thread does (it receives the cipher state in the Join record).
#pragma once

#include <liburing.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "base/buf.hpp"
#include "base/spsc.hpp"
#include "crypto/cfb8.hpp"
#include "proto/frame.hpp"

namespace net {

enum class Phase : u8 { Handshake, Status, Login, Config, Play };

// ---- net -> tick records (payloads of Reactor::inbound()) --------------------------------
enum class InKind : u32 { Packet, Join, Leave };
struct InHeader {
    u32 slot;
    u32 gen;
    InKind kind;
    u32 reserved;
};  // followed by: Packet -> packet body (id + payload); Join -> opaque join blob; Leave -> nothing

// Join records carry the outbound transport state right after the header, then the sink's blob.
struct JoinTransport {
    crypto::Cfb8 enc;
    i32 threshold;
    u32 encrypted;
};

// ---- tick -> net messages ----------------------------------------------------------------
enum class OutKind : u32 {
    Send,       // queue buf for sending
    SendClose,  // queue buf (may be null), then close once everything is flushed
    Release,    // tick thread has forgotten the player: the slot may be reused
};
struct OutMsg {
    u32 slot;
    u32 gen;
    OutKind kind;
    base::ByteBuf* buf;
};

struct Conn {
    // -- hot: touched on every recv/send completion
    int fd = -1;
    u32 slot = 0;
    u32 gen = 1;
    Phase phase = Phase::Handshake;
    bool open = false;
    bool encrypted = false;
    bool recv_armed = false;
    bool send_busy = false;
    bool closing = false;
    bool close_after_send = false;  // flush what is queued, then linger
    bool lingering = false;         // write side shut down; waiting for the peer's FIN (or the deadline)
    bool zombie = false;            // closed while in play; waiting for the tick thread's Release
    bool dirty = false;             // has staged pre-play output to flush
    bool crypt_switched = false;    // encryption was enabled while a recv buffer was being parsed
    i32 threshold = proto::kNoCompression;
    u32 max_frame = 0;
    base::ByteBuf* send_head = nullptr;
    base::ByteBuf* send_tail = nullptr;
    base::ByteBuf in;  // bytes of a frame that straddles recv buffers
    crypto::Cfb8 dec;
    // -- pre-play only
    crypto::Cfb8 enc;
    base::ByteBuf out;    // staged pre-play output (plaintext until flushed)
    u64 deadline_ns = 0;  // must reach play (or finish closing) by then
    void* pre = nullptr;  // PreSink's per-connection state
    std::string remote;   // "ip:port", for logs
};

// Protocol logic for handshake/status/login/configuration. Called on the reactor thread.
class PreSink {
  public:
    virtual void on_open(class Reactor&, Conn&) = 0;
    virtual void on_frame(class Reactor&, Conn&, std::span<const u8> body) = 0;
    virtual void on_close(class Reactor&, Conn&) = 0;  // only for connections that never reached play
  protected:
    ~PreSink() = default;
};

// base::ByteBuf::tag values: which thread's pool a sent buffer goes back to.
inline constexpr u32 kBufOwnerNet = 0;
inline constexpr u32 kBufOwnerTick = 1;

struct ReactorConfig {
    std::string bind_ip = "0.0.0.0";
    u16 port = 25565;
    u32 max_conns = 2048;
    u32 recv_bufs = 2048;  // provided-buffer ring entries (power of two)
    u32 recv_buf_size = 2048;
    u32 inbound_ring_bytes = 8u << 20;
    u64 prelogin_timeout_ns = 30ull * 1'000'000'000ull;
};

class Reactor {
  public:
    Reactor(const ReactorConfig& cfg, PreSink& sink, u32 index);
    ~Reactor();
    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    bool listen(std::string& err);  // bind + listen; call before run()
    void run();                     // blocks until stop(); call on the reactor's own thread
    void stop();                    // any thread

    u32 index() const { return index_; }
    u32 max_conns() const { return cfg_.max_conns; }

    // ---- tick-thread side -----------------------------------------------------------------
    base::SpscBytes& inbound() { return inbound_; }
    base::SpscRing<base::ByteBuf*>& recycled() { return recycled_; }
    bool post(const OutMsg& m) { return outbox_.push(m); }
    void wake();  // any thread; coalesces with other pending wakes

    // ---- any thread: run fn on the reactor thread (cold path: auth results, admin actions) --
    void run_on_loop(std::function<void()> fn);

    // ---- [loop] API for PreSink -------------------------------------------------------------
    Conn* lookup(u32 slot, u32 gen) {
        if (slot >= cfg_.max_conns) return nullptr;
        Conn& c = conns_[slot];
        return (c.open && c.gen == gen && !c.closing) ? &c : nullptr;
    }
    template <class P>
    void send(Conn& c, const P& pkt) {
        proto::write_packet(c.out, c.threshold, pkt);
        mark_dirty(c);
    }
    void send_raw_frames(Conn& c, std::span<const u8> framed) {  // bytes framed for c.threshold already
        c.out.append(framed.data(), framed.size());
        mark_dirty(c);
    }
    void enable_encryption(Conn& c, const u8 key[16]);
    void set_compression(Conn& c, i32 threshold) { c.threshold = threshold; }
    void set_phase(Conn& c, Phase p);
    void close(Conn& c);              // drop now
    void close_after_flush(Conn& c);  // send what is staged/queued, then drop
    // Hand the connection to the tick thread: emits a Join record [InHeader][JoinTransport][blob].
    // From here on the reactor never writes this connection's outbound stream again.
    bool promote_to_play(Conn& c, std::span<const u8> blob);

  private:
    enum Op : u64 { kAccept = 1, kRecv, kSend, kWake };
    static u64 tag(Op op, const Conn& c) {
        return (static_cast<u64>(op) << 56) | (static_cast<u64>(c.gen & 0xFFFFFF) << 32) | c.slot;
    }

    io_uring_sqe* sqe();
    void arm_accept();
    void arm_recv(Conn& c);
    void arm_wake();
    void on_accept(int res, u32 flags);
    void on_recv(Conn& c, io_uring_cqe* cqe);
    void on_send(Conn& c, int res);
    void process(Conn& c, u8* data, u32 n);
    void mark_dirty(Conn& c) {
        if (!c.dirty) {
            c.dirty = true;
            dirty_.push_back(c.slot);
        }
    }
    void flush_staged(Conn& c);
    void queue_send(Conn& c, base::ByteBuf* b);
    void start_send(Conn& c);
    void finish_close(Conn& c);
    void begin_linger(Conn& c);
    void return_buf(u32 bid);
    void free_slot(Conn& c);
    void recycle(base::ByteBuf* b);
    void drain_outbox();
    void drain_tasks();
    void housekeeping(u64 now);
    bool push_leave(const Conn& c);

    ReactorConfig cfg_;
    PreSink& sink_;
    u32 index_;
    int listen_fd_ = -1;
    int wake_fd_ = -1;
    u64 wake_val_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> wake_pending_{false};

    io_uring ring_{};
    bool ring_ok_ = false;
    io_uring_buf_ring* br_ = nullptr;
    u8* recv_mem_ = nullptr;
    u32 br_pending_ = 0;  // buffers re-added but not yet advanced
    bool accept_armed_ = false;

    std::vector<Conn> conns_;
    std::vector<u32> free_slots_;
    std::vector<u32> dirty_;
    std::vector<u32> rearm_;          // recv to re-arm after buffers are returned (-ENOBUFS)
    std::vector<u32> pending_leave_;  // Leave records that did not fit in the inbound ring
    base::BufPool pool_;              // reactor-owned buffers (pre-play output)

    base::SpscBytes inbound_;                  // net -> tick
    base::SpscRing<base::ByteBuf*> recycled_;  // net -> tick: tick-owned buffers, sent
    base::SpscRing<OutMsg> outbox_;            // tick -> net

    std::mutex task_mu_;
    std::vector<std::function<void()>> tasks_;
    std::atomic<bool> tasks_pending_{false};
};

}  // namespace net
