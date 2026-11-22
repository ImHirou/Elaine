// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/console.hpp"

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "base/log.hpp"

namespace server {

Console::Console() : stop_fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

Console::~Console() {
    if (running_) {
        const u64 one = 1;
        if (write(stop_fd_, &one, sizeof one) < 0) {}  // wakes the poll
        pthread_join(thread_, nullptr);
    }
    if (stop_fd_ >= 0) close(stop_fd_);
}

void Console::start() {
    if (stop_fd_ < 0) return;
    auto start = [](void* self) -> void* {
        pthread_setname_np(pthread_self(), "console");
        static_cast<Console*>(self)->run();
        return nullptr;
    };
    const int err = pthread_create(&thread_, nullptr, start, this);
    running_ = err == 0;
    if (!running_) LOG_WARN("console: no thread to read it (%s); the server goes on without one", std::strerror(err));
}

void Console::run() {
    std::string pending;
    char buf[4096];
    bool overlong = false;  // the line being read has outgrown kMaxLine: skip to its end
    while (true) {
        pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents) return;
        const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;  // end of input, or not ours to read (EIO: a background job)
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c != '\n') {
                if (pending.size() < kMaxLine)
                    pending += c;
                else
                    overlong = true;
                continue;
            }
            if (!pending.empty() && pending.back() == '\r') pending.pop_back();
            if (!overlong && !pending.empty()) {
                // A full ring means the tick thread is not running: the line is dropped.
                if (u8* p = lines_.alloc(static_cast<u32>(pending.size()))) {
                    std::memcpy(p, pending.data(), pending.size());
                    lines_.commit();
                    lines_.publish();
                }
            }
            pending.clear();
            overlong = false;
        }
    }
}

bool Console::next(std::string& line) {
    const u8* payload;
    u32 n;
    if (!lines_.front(payload, n)) return false;
    line.assign(reinterpret_cast<const char*>(payload), n);
    lines_.pop(n);
    lines_.release();
    return true;
}

}  // namespace server
