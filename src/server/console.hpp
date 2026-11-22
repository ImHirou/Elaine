// SPDX-License-Identifier: GPL-3.0-or-later
// The server console: lines typed on stdin, handed to the tick thread to be run as commands
// (vanilla's "Server console handler" thread and DedicatedServer.handleConsoleInputs).
//
// A thread of its own blocks in poll() on stdin; each line travels through an SPSC ring, as
// everything between threads does. When stdin ends (a service started with </dev/null) or
// cannot be read (a background job without the terminal), the thread ends and the server
// goes on without a console.
#pragma once

#include <pthread.h>

#include <string>

#include "base/spsc.hpp"

namespace server {

class Console {
  public:
    Console();
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    void start();
    // Tick thread: the next line that was typed, if there is one.
    bool next(std::string& line);

  private:
    static constexpr u32 kMaxLine = 32500;  // as long as a command a client may send
    void run();

    base::SpscBytes lines_{1u << 17};
    pthread_t thread_{};  // (not a std::thread: <thread> is 0.9 s of every file that includes this one)
    bool running_ = false;
    int stop_fd_ = -1;
};

}  // namespace server
