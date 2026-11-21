// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "base/log.hpp"

namespace server {

struct Files::ReadJob {
    std::string path;
    void* ctx;
    u64 tag;
    Done done;
    std::vector<u8> bytes;
    bool found = false;
    std::string error;
};

struct Files::WriteJob {
    Files* files;
    std::string path;
    std::vector<u8> bytes;  // the job's own: `latest` may be replaced while this is out
    bool sync;
    std::string error;
};

void Files::read(const std::string& path, void* ctx, u64 tag, Done done) {
    if (const auto it = writing_.find(path); it != writing_.end()) {
        done(ctx, tag, it->second.latest, true);
        return;
    }
    auto* job = new ReadJob{path, ctx, tag, done, {}, false, {}};
    ++jobs_;
    workers_.submit({&read_work, &read_done, this, job, 0, 0, 0, 0});
}

void Files::write(const std::string& path, std::span<const u8> bytes) {
    const auto [it, fresh] = writing_.try_emplace(path);
    Entry& e = it->second;
    e.latest.assign(bytes.begin(), bytes.end());
    if (fresh)
        start_write(path, e);
    else
        e.stale = true;  // goes out when the write before it is back
}

void Files::start_write(const std::string& path, const Entry& e) {
    auto* job = new WriteJob{this, path, e.latest, sync_, {}};
    ++jobs_;
    workers_.submit({&write_work, &write_done, this, job, 0, 0, 0, 0});
}

void Files::finish() {
    while (jobs_) workers_.drain();  // (a write that comes back may send the next one out)
}

void Files::read_work(base::Task& t) {
    auto* job = static_cast<ReadJob*>(t.data);
    const int fd = ::open(job->path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) job->error = std::strerror(errno);
        return;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0 || static_cast<usize>(st.st_size) > kMaxSize) {
        job->error = "not a file of this server";
        ::close(fd);
        return;
    }
    job->bytes.resize(static_cast<usize>(st.st_size));
    usize got = 0;
    while (got < job->bytes.size()) {
        const ssize_t n = ::read(fd, job->bytes.data() + got, job->bytes.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<usize>(n);
    }
    ::close(fd);
    if (got == job->bytes.size())
        job->found = true;
    else
        job->error = "short read";
}

void Files::read_done(base::Task& t) {
    auto* job = static_cast<ReadJob*>(t.data);
    Files& f = *static_cast<Files*>(t.ctx);
    --f.jobs_;
    if (!job->error.empty()) LOG_ERROR("cannot read %s: %s", job->path.c_str(), job->error.c_str());
    job->done(job->ctx, job->tag, job->bytes, job->found);
    delete job;
}

void Files::write_work(base::Task& t) {
    auto* job = static_cast<WriteJob*>(t.data);
    const std::string tmp = job->path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        job->error = std::strerror(errno);
        return;
    }
    usize put = 0;
    while (put < job->bytes.size()) {
        const ssize_t n = ::write(fd, job->bytes.data() + put, job->bytes.size() - put);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        put += static_cast<usize>(n);
    }
    int err = 0;
    if (put != job->bytes.size())
        err = errno ? errno : EIO;
    else if (job->sync && ::fdatasync(fd) != 0)
        err = errno;
    ::close(fd);
    if (!err && std::rename(tmp.c_str(), job->path.c_str()) != 0) err = errno;
    if (err) {
        job->error = std::strerror(err);
        ::unlink(tmp.c_str());
    }
}

void Files::write_done(base::Task& t) {
    auto* job = static_cast<WriteJob*>(t.data);
    Files& f = *job->files;
    --f.jobs_;
    if (!job->error.empty()) LOG_ERROR("cannot write %s: %s", job->path.c_str(), job->error.c_str());
    const auto it = f.writing_.find(job->path);
    if (it != f.writing_.end()) {
        if (it->second.stale) {
            it->second.stale = false;
            f.start_write(it->first, it->second);
        } else {
            f.writing_.erase(it);
        }
    }
    delete job;
}

}  // namespace server
