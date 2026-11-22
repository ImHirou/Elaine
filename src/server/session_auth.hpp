// SPDX-License-Identifier: GPL-3.0-or-later
// Online-mode profile verification: blocking HTTPS calls to the session server on a small
// worker pool, so reactor threads never wait on the network. Cold path (once per login).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/types.hpp"

namespace server {

struct Property {
    std::string name, value, signature;
};

struct AuthResult {
    bool ok = false;
    Uuid uuid;
    std::string name;
    std::vector<Property> props;
    std::string error;
};

class SessionAuth {
  public:
    explicit SessionAuth(std::string base_url, unsigned threads = 2);
    ~SessionAuth();
    SessionAuth(const SessionAuth&) = delete;
    SessionAuth& operator=(const SessionAuth&) = delete;

    // `done` runs on a worker thread; hop back with Reactor::run_on_loop.
    void verify(std::string username, std::string server_hash, std::function<void(AuthResult&&)> done);

  private:
    // The threads, their queue and its lock - in session_auth.cpp: <thread>, <mutex> and
    // <condition_variable> are a second of every file that includes this one.
    struct Pool;
    std::unique_ptr<Pool> pool_;
};

// Parses a hasJoined response body. Exposed for tests.
bool parse_profile_json(const std::string& body, AuthResult& out);

}  // namespace server
