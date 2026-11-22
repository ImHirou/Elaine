// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/session_auth.hpp"

#include <curl/curl.h>
#include <yyjson.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "crypto/auth.hpp"

namespace server {

namespace {

size_t on_body(char* data, size_t size, size_t nmemb, void* user) {
    auto* out = static_cast<std::string*>(user);
    if (out->size() + size * nmemb > (1u << 20)) return 0;  // a profile is a few KB; refuse anything silly
    out->append(data, size * nmemb);
    return size * nmemb;
}

}  // namespace

bool parse_profile_json(const std::string& body, AuthResult& out) {
    yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0);
    if (!doc) return false;
    bool ok = false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    const char* id = yyjson_get_str(yyjson_obj_get(root, "id"));
    const char* name = yyjson_get_str(yyjson_obj_get(root, "name"));
    if (id && name && crypto::uuid_from_string(id, out.uuid)) {
        out.name = name;
        yyjson_val* props = yyjson_obj_get(root, "properties");
        size_t idx, max;
        yyjson_val* p;
        yyjson_arr_foreach(props, idx, max, p) {
            const char* pn = yyjson_get_str(yyjson_obj_get(p, "name"));
            const char* pv = yyjson_get_str(yyjson_obj_get(p, "value"));
            const char* ps = yyjson_get_str(yyjson_obj_get(p, "signature"));
            if (pn && pv) out.props.push_back({pn, pv, ps ? ps : ""});
        }
        ok = !out.name.empty() && out.name.size() <= 16;
    }
    yyjson_doc_free(doc);
    return ok;
}

struct SessionAuth::Pool {
    struct Job {
        std::string username, hash;
        std::function<void(AuthResult&&)> done;
    };
    void worker();

    std::string base_url;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool stop = false;
    std::vector<std::thread> threads;
};

SessionAuth::SessionAuth(std::string base_url, unsigned threads) : pool_(std::make_unique<Pool>()) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pool_->base_url = std::move(base_url);
    while (!pool_->base_url.empty() && pool_->base_url.back() == '/') pool_->base_url.pop_back();
    for (unsigned i = 0; i < threads; ++i) pool_->threads.emplace_back([p = pool_.get()] { p->worker(); });
}

SessionAuth::~SessionAuth() {
    {
        std::lock_guard<std::mutex> lk(pool_->mu);
        pool_->stop = true;
    }
    pool_->cv.notify_all();
    for (auto& t : pool_->threads) t.join();
    curl_global_cleanup();
}

void SessionAuth::verify(std::string username, std::string server_hash, std::function<void(AuthResult&&)> done) {
    {
        std::lock_guard<std::mutex> lk(pool_->mu);
        pool_->jobs.push_back({std::move(username), std::move(server_hash), std::move(done)});
    }
    pool_->cv.notify_one();
}

void SessionAuth::Pool::worker() {
    CURL* curl = curl_easy_init();  // reused: keeps the TLS connection to the session server warm
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [this] { return stop || !jobs.empty(); });
            if (stop) break;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        AuthResult res;
        std::string body;
        char* esc = curl_easy_escape(curl, job.username.c_str(), static_cast<int>(job.username.size()));
        const std::string url = base_url + "/session/minecraft/hasJoined?username=" + (esc ? esc : "") + "&serverId=" + job.hash;
        curl_free(esc);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (rc != CURLE_OK) {
            res.error = std::string("session server unreachable: ") + curl_easy_strerror(rc);
        } else if (status != 200) {
            res.error = "session server rejected the login (HTTP " + std::to_string(status) + ")";
        } else if (!parse_profile_json(body, res)) {
            res.error = "malformed session server response";
        } else {
            res.ok = true;
        }
        job.done(std::move(res));
    }
    curl_easy_cleanup(curl);
}

}  // namespace server
