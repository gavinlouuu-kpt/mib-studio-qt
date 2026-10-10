#include "backend/pz/SsdStore.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace backend::pz {

namespace {
struct NamedState { const char* name; SsdState state; };
constexpr NamedState kStates[] = {
    {"ABSENT", SsdState::Absent},           {"INITIALISING", SsdState::Initialising}, {"RECOVERING", SsdState::Recovering},
    {"READY", SsdState::Ready},             {"RECORDING", SsdState::Recording},       {"STOPPING", SsdState::Stopping},
    {"WEDGED", SsdState::Wedged},           {"RUN_TABLE_FULL", SsdState::RunTableFull}, {"RAW_FULL", SsdState::RawFull},
    {"UNFORMATTED", SsdState::Unformatted},
};

using nlohmann::json;

bool getU64(const json& j, const char* key, uint64_t& out, std::string* why) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) {
        if (why) *why = std::string("pzrec JSON: field '") + key + "' is missing or not an unsigned number";
        return false;
    }
    out = it->get<uint64_t>();
    return true;
}
bool getU32(const json& j, const char* key, uint32_t& out, std::string* why) {
    uint64_t v = 0;
    if (!getU64(j, key, v, why)) return false;
    if (v > 0xFFFFFFFFull) {
        if (why) *why = std::string("pzrec JSON: field '") + key + "' does not fit 32 bits";
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}
bool getBool(const json& j, const char* key, bool& out, std::string* why) {
    auto it = j.find(key);
    if (it == j.end() || !(it->is_boolean() || it->is_number_unsigned())) {
        if (why) *why = std::string("pzrec JSON: field '") + key + "' is missing or not a boolean";
        return false;
    }
    out = it->is_boolean() ? it->get<bool>() : it->get<uint64_t>() != 0;
    return true;
}
} // namespace

const char* ssdStateName(SsdState s) {
    for (const auto& n : kStates) if (n.state == s) return n.name;
    return "?";
}

bool parseSsdState(const std::string& name, SsdState& out) {
    for (const auto& n : kStates) {
        if (name == n.name) { out = n.state; return true; }
    }
    return false;
}

bool parseSsdStatus(const std::string& text, SsdStatus& out, std::string* why) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        if (why) *why = "pzrec status is not a JSON object";
        return false;
    }
    if (auto e = j.find("error"); e != j.end()) {
        if (why) *why = "pzrec reported an error: " + (e->is_string() ? e->get<std::string>() : e->dump());
        return false;
    }
    SsdStatus s;
    auto st = j.find("state");
    if (st == j.end() || !st->is_string() || !parseSsdState(st->get<std::string>(), s.state)) {
        if (why) *why = "pzrec status: unknown or missing state";
        return false;
    }
    if (auto le = j.find("last_error"); le != j.end() && le->is_string()) s.lastError = le->get<std::string>();
    if (!getU64(j, "raw_sectors", s.rawSectors, why) || !getU64(j, "head_lba", s.headLba, why) || !getU64(j, "tail_lba", s.tailLba, why) ||
        !getU64(j, "free_sectors", s.freeSectors, why) || !getU64(j, "min_run_sectors", s.minRunSectors, why) ||
        !getU32(j, "runs", s.runs, why) || !getU32(j, "next_run_id", s.nextRunId, why) || !getU32(j, "table_entries", s.tableEntries, why) ||
        !getBool(j, "open_run", s.openRun, why) || !getU32(j, "open_run_id", s.openRunId, why) ||
        !getU64(j, "open_start_unix_ms", s.openStartUnixMs, why) || !getU32(j, "open_client_tag", s.openClientTag, why) ||
        !getU32(j, "open_wall_source", s.openWallSource, why) || !getU32(j, "recovered_runs", s.recoveredRuns, why) ||
        !getU32(j, "skipped_bad_entries", s.skippedBadEntries, why)) {
        return false;
    }
    if (auto ids = j.find("recovered_ids"); ids != j.end() && ids->is_array()) {
        for (const auto& v : *ids) {
            if (v.is_number_unsigned() && v.get<uint64_t>() != 0 && v.get<uint64_t>() <= 0xFFFFFFFFull) s.recoveredIds.push_back(v.get<uint32_t>());
        }
    }
    auto c = j.find("counters");
    if (c == j.end() || !c->is_object()) {
        if (why) *why = "pzrec status: no counters";
        return false;
    }
    if (!getU64(*c, "seen", s.counters.seen, why) || !getU64(*c, "empty_filtered", s.counters.emptyFiltered, why) ||
        !getU64(*c, "invalid_not_sampled", s.counters.invalidNotSampled, why) || !getU64(*c, "passed", s.counters.passed, why) ||
        !getU64(*c, "written", s.counters.written, why) || !getU64(*c, "dropped", s.counters.dropped, why) ||
        !getU64(*c, "failed", s.counters.failed, why) || !getU64(*c, "bytes_written", s.counters.bytesWritten, why) ||
        !getU64(*c, "drain_kbps", s.counters.drainKbps, why) || !getU64(*c, "first_frame_id", s.counters.firstFrameId, why) ||
        !getU64(*c, "last_frame_id", s.counters.lastFrameId, why)) {
        return false;
    }
    out = std::move(s);
    return true;
}

bool parseSsdRuns(const std::string& text, std::vector<SsdRun>& out, std::string* why) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        if (why) {
            *why = "pzrec runs is not a JSON array";
            if (j.is_object() && j.contains("error") && j["error"].is_string()) *why = "pzrec reported an error: " + j["error"].get<std::string>();
        }
        return false;
    }
    std::vector<SsdRun> runs;
    for (const auto& e : j) {
        if (!e.is_object()) {
            if (why) *why = "pzrec runs: an entry is not an object";
            return false;
        }
        SsdRun r;
        if (!getU32(e, "run_id", r.id, why) || !getBool(e, "open", r.open, why) || !getBool(e, "deleted", r.deleted, why) ||
            !getBool(e, "incomplete", r.incomplete, why) || !getU64(e, "start_lba", r.startLba, why) || !getU64(e, "end_lba", r.endLba, why) ||
            !getU64(e, "start_unix_ms", r.startUnixMs, why) || !getU32(e, "wall_source", r.wallSource, why) ||
            !getU32(e, "client_tag", r.clientTag, why) || !getU32(e, "filter", r.filter, why) || !getU32(e, "sampler_n", r.samplerN, why) ||
            !getU64(e, "rec_sectors", r.recSectors, why) || !getU64(e, "first_frame_id", r.firstFrameId, why) ||
            !getU64(e, "last_frame_id", r.lastFrameId, why) || !getU64(e, "first_ticks", r.firstTicks, why) ||
            !getU64(e, "last_ticks", r.lastTicks, why) || !getU32(e, "tick_hz", r.tickHz, why) || !getU64(e, "seen", r.seen, why) ||
            !getU64(e, "empty_filtered", r.emptyFiltered, why) || !getU64(e, "invalid_not_sampled", r.invalidNotSampled, why) ||
            !getU64(e, "passed", r.passed, why) || !getU64(e, "written", r.written, why) || !getU64(e, "dropped", r.dropped, why) ||
            !getU64(e, "failed", r.failed, why) || !getU32(e, "recoveries", r.recoveries, why) || !getU32(e, "reason", r.reason, why) ||
            !getBool(e, "counts_unknown", r.countsUnknown, why)) {
            return false;
        }
        // A table the library could not have written is refused whole: a list the UI cannot trust is worse than none.
        if (r.endLba < r.startLba) {
            if (why) *why = "pzrec runs: run " + std::to_string(r.id) + " ends before it starts";
            return false;
        }
        for (const auto& other : runs) {
            if (other.id == r.id) {
                if (why) *why = "pzrec runs: duplicate run id " + std::to_string(r.id);
                return false;
            }
        }
        // The totals must add up: seen = empty + invalid not sampled + passed, passed = written + dropped + failed. A run that does not add up shows
        // no numbers (the same "—" as a run with unknown totals) and says so.
        if (!r.countsUnknown && !r.open &&
            (r.seen != r.emptyFiltered + r.invalidNotSampled + r.passed || r.passed != r.written + r.dropped + r.failed)) {
            r.totalsInconsistent = true;
        }
        runs.push_back(r);
    }
    out = std::move(runs);
    return true;
}

namespace {
#if !defined(_WIN32)
bool makePipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    for (int i = 0; i < 2; ++i) fcntl(fds[i], F_SETFD, fcntl(fds[i], F_GETFD) | FD_CLOEXEC);
    return true;
}
#endif
} // namespace

RunResult runBounded(const std::vector<std::string>& argv, std::chrono::milliseconds timeout) {
    RunResult r;
#if defined(_WIN32)
    (void)argv; (void)timeout;
    r.err = "pzrec is not available on this platform";
    return r;
#else
    if (argv.empty()) { r.err = "no command"; return r; }
    int outPipe[2], errPipe[2];
    if (!makePipe(outPipe)) { r.err = std::string("pipe: ") + std::strerror(errno); return r; }
    if (!makePipe(errPipe)) {
        r.err = std::string("pipe: ") + std::strerror(errno);
        close(outPipe[0]); close(outPipe[1]);
        return r;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errPipe[1], 2);
    std::vector<char*> cargv;
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(outPipe[1]);
    close(errPipe[1]);
    if (rc != 0) {
        r.err = argv[0] + ": " + std::strerror(rc);
        close(outPipe[0]); close(errPipe[0]);
        return r;
    }
    r.started = true;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int fds[2] = {outPipe[0], errPipe[0]};
    bool open[2] = {true, true};
    for (int fd : fds) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    constexpr size_t kMaxCapture = 4u << 20;  // a run table of 256 entries is about 60 KB
    auto drain = [&](int i) {
        char buf[4096];
        for (;;) {
            const ssize_t got = read(fds[i], buf, sizeof buf);
            if (got > 0) {
                std::string& dst = i == 0 ? r.out : r.err;
                if (dst.size() < kMaxCapture) dst.append(buf, static_cast<size_t>(got));
                else r.truncated = true;
                continue;
            }
            if (got < 0 && (errno == EINTR)) continue;
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            open[i] = false;  // EOF or a hard error
            return;
        }
    };
    int status = 0;
    bool exited = false;
    // The child's exit, not the pipes, ends the wait: a child that closes its output and keeps running, or a grandchild that inherits the pipes and
    // outlives it, must not hold us past the deadline. Every wait below is bounded by the deadline; the last resort is SIGKILL.
    while (!exited) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        pollfd p[2];
        int n = 0, idx[2];
        for (int i = 0; i < 2; ++i) if (open[i]) { p[n] = {fds[i], POLLIN, 0}; idx[n++] = i; }
        if (n > 0) {
            const int pr = poll(p, n, static_cast<int>(std::min<long long>(left.count(), 50)));
            if (pr > 0) for (int k = 0; k < n; ++k) if (p[k].revents & (POLLIN | POLLHUP | POLLERR)) drain(idx[k]);
            // pr < 0 (not EINTR) or a closed pipe: fall through to the bounded wait for the exit below
            if (pr < 0 && errno != EINTR) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) exited = true;
        else if (w < 0 && errno != EINTR) { exited = true; status = 0; }
    }
    if (exited) {
        for (int i = 0; i < 2; ++i) if (open[i]) drain(i);  // what the child wrote before it exited
        r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else {
        r.timedOut = true;
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    close(outPipe[0]);
    close(errPipe[0]);
    return r;
#endif
}

PzrecCliDevice::PzrecCliDevice(std::string pzrecPath, std::string imagePath, std::chrono::milliseconds timeout, std::vector<std::string> extraArgs,
                               std::chrono::milliseconds startTimeout, std::chrono::milliseconds stopTimeout)
    : pzrec_(std::move(pzrecPath)), image_(std::move(imagePath)), timeout_(timeout), startTimeout_(startTimeout), stopTimeout_(stopTimeout),
      extra_(std::move(extraArgs)) {}

bool PzrecCliDevice::call(const std::vector<std::string>& verbAndArgs, std::chrono::milliseconds timeout, std::string& json, std::string* error) {
    const std::string verb = verbAndArgs.empty() ? std::string() : verbAndArgs.front();
    std::vector<std::string> argv{pzrec_, verb, image_};
    argv.insert(argv.end(), verbAndArgs.begin() + (verbAndArgs.empty() ? 0 : 1), verbAndArgs.end());
    argv.insert(argv.end(), extra_.begin(), extra_.end());
    const RunResult r = runBounded(argv, timeout);
    auto firstLine = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.size() > 300) s.resize(300);
        return s;
    };
    if (!r.started) { if (error) *error = "pzrec could not be run: " + firstLine(r.err); return false; }
    if (r.timedOut) { if (error) *error = "pzrec " + verb + " did not answer within " + std::to_string(timeout.count()) + " ms"; return false; }
    if (r.truncated) { if (error) *error = "pzrec " + verb + " wrote more than 4 MiB: refused"; return false; }
    if (r.exitCode != 0) {
        if (error) {
            *error = "pzrec " + verb + " exited with " + std::to_string(r.exitCode);
            const std::string detail = firstLine(r.err.empty() ? r.out : r.err);
            if (!detail.empty()) *error += ": " + detail;
        }
        return false;
    }
    json = r.out;
    return true;
}

bool PzrecCliDevice::status(std::string& json, std::string* error) { return call({"status"}, timeout_, json, error); }
bool PzrecCliDevice::runs(std::string& json, std::string* error) { return call({"runs"}, timeout_, json, error); }

bool PzrecCliDevice::start(const SsdStartArgs& a, std::string& json, std::string* error) {
    const char* filter = a.filter == SsdFilter::All ? "all" : a.filter == SsdFilter::AnyResult ? "any" : "valid";
    return call({"start", "--filter", filter, "--sampler", std::to_string(a.samplerN), "--start-ms", std::to_string(a.startUnixMs), "--wall-source",
                 a.clientSynced ? "1" : "0", "--tag", std::to_string(a.clientTag)},
                startTimeout_, json, error);
}

bool PzrecCliDevice::windowSnapshot(std::string& json, std::string* error) {
    return call({"snapshot", "--window-only"}, timeout_, json, error);
}

bool PzrecCliDevice::readArgv(uint32_t run, uint64_t from, uint64_t count, std::vector<std::string>& argv) {
    // pzrec read RUN IMG <the same options as start/stop> --summary [--from F] [--count N]: RUN comes before the image; --summary makes a clean finish
    // report its exact byte count on stderr.
    argv = {pzrec_, "read", std::to_string(run), image_};
    argv.insert(argv.end(), extra_.begin(), extra_.end());
    argv.push_back("--summary");
    if (from != 0) { argv.push_back("--from"); argv.push_back(std::to_string(from)); }
    if (count != 0) { argv.push_back("--count"); argv.push_back(std::to_string(count)); }
    return true;
}

bool PzrecCliDevice::stop(bool abort, std::string& json, std::string* error) {
    std::vector<std::string> v{"stop"};
    if (abort) v.push_back("--abort");
    return call(v, stopTimeout_, json, error);
}

SsdStore::SsdStore(std::unique_ptr<ISsdDevice> device, std::chrono::milliseconds interval, bool background, std::string stateFile)
    : device_(std::move(device)), interval_(interval), background_(background && device_ != nullptr), stateFile_(std::move(stateFile)) {
    if (background_) {
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(m_);
            while (!stop_) {
                lock.unlock();
                refresh(false);
                lock.lock();
                cv_.wait_for(lock, interval_, [this] { return stop_; });
            }
        });
    }
}

SsdStore::~SsdStore() {
    shuttingDown_ = true;
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
    }
    stopCv_.notify_all();
    if (stopThread_.joinable()) stopThread_.join();
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

namespace {
const char* reasonFor(SsdState s) {
    switch (s) {
        case SsdState::Absent: return "No SSD: nothing can be saved";
        case SsdState::Initialising: return "SSD starting";
        case SsdState::Recovering: return "SSD recovering";
        case SsdState::Wedged: return "SSD not responding. The drive is self-powered; power-cycle it.";
        case SsdState::RunTableFull: return "SSD run table full: delete old runs";
        case SsdState::RawFull: return "SSD raw area full: export or delete old runs";
        case SsdState::Unformatted: return "SSD is not formatted for recording";
        default: return "";
    }
}
// What a change of the table would show: the log pointers and the open run.
uint64_t signatureOf(const SsdStatus& s) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : {static_cast<uint64_t>(s.runs), static_cast<uint64_t>(s.nextRunId), s.headLba, s.tailLba, static_cast<uint64_t>(s.openRun),
                       static_cast<uint64_t>(s.recoveredRuns), static_cast<uint64_t>(s.skippedBadEntries)}) {
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}
uint32_t readSeenId(const std::string& path) {
    if (path.empty()) return 0;
    std::ifstream in(path);
    if (!in) return 0;
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return 0;
    auto it = j.find("last_seen_run_id");
    if (it == j.end() || !it->is_number_unsigned() || it->get<uint64_t>() > 0xFFFFFFFFull) return 0;
    return it->get<uint32_t>();
}

// Atomic and durable (a temporary file, fsync, rename, fsync of the directory): a power loss leaves the old value or the new one, never half of it.
void writeSeenId(const std::string& path, uint32_t id) {
    if (path.empty()) return;
    std::error_code ec;
    const std::filesystem::path target(path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    const std::string tmp = path + ".tmp";
    const std::string body = json{{"last_seen_run_id", id}}.dump() + "\n";
#if !defined(_WIN32)
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    bool ok = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::filesystem::remove(tmp, ec); return; }
    const std::string dir = target.has_parent_path() ? target.parent_path().string() : std::string(".");
    const int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
#else
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << body;
        out.flush();
        if (!out) { std::filesystem::remove(tmp, ec); return; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
#endif
}
} // namespace

SsdStatus SsdStore::statusLocked() const {
    if (!device_) {
        SsdStatus s;
        s.state = SsdState::Absent;
        s.reason = reasonFor(SsdState::Absent);
        return s;
    }
    if (!haveStatus_) {
        SsdStatus s;
        s.state = SsdState::Initialising;
        s.reason = reasonFor(SsdState::Initialising);
        return s;
    }
    if (stopRunning_) {
        SsdStatus s = status_;
        s.state = SsdState::Stopping;
        s.reason = "Stopping the SSD run";
        return s;
    }
    return status_;
}

void SsdStore::refresh(bool force) {
    if (!device_) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (exportHeldLocked()) return;          // the cache stays as it was; the export owns the disk
        if (openedRunId_ != 0 && !stopRunning_ && !stopFailed_) {
            // Studio's own run is open: while a run is active the PL's admission gate holds back every read and write of the disk (`pzrec status`, `runs`,
            // `delete`, a second `start`; board owner, 2026-10-10), so pzrec is not called at all, not even to be refused. The state is RECORDING with the
            // run Studio opened; the live counters need a window-only read (`snapshot`), which the CLI does not offer yet. Never READY.
            status_.state = SsdState::Recording;
            status_.openRun = true;
            status_.openRunId = openedRunId_;
            status_.reason = "Recording; live counters are not read while the run is active (the disk is held back)";
            haveStatus_ = true;
            statusAt_ = std::chrono::steady_clock::now();
            return;
        }
        if (!force && haveStatus_ && std::chrono::steady_clock::now() - statusAt_ < interval_) return;
    }
    // The pzrec calls run without the lock held: a slow child never blocks status() readers. They are serialised on deviceMutex_: a start or a stop in
    // progress owns the window and the disk, so a periodic refresh that finds the device busy skips its turn (a forced one waits).
    std::unique_lock<std::mutex> device(deviceMutex_, std::defer_lock);
    if (force) device.lock();
    else if (!device.try_lock()) return;
    {
        // An export owns the window and the disk (one bounce buffer): stand down, also when the lease was taken after the check above.
        std::lock_guard<std::mutex> lock(m_);
        if (exportHeldLocked()) return;
    }
    SsdStatus s;
    std::string text, error;
    const bool ok = device_->status(text, &error) && parseSsdStatus(text, s, &error);
    if (!ok) {
        s = SsdStatus{};
        s.state = SsdState::Wedged;
        s.reason = "SSD not responding: " + error;
    } else {
        s.reason = reasonFor(s.state);
    }
    bool wantRuns = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        ++ticksSinceRuns_;
        wantRuns = ok && s.state != SsdState::Unformatted && s.state != SsdState::Initialising && s.state != SsdState::Wedged && s.state != SsdState::Absent &&
                   (!haveRuns_ || signatureOf(s) != runsSignature_ || ticksSinceRuns_ >= 10 || force);
    }
    std::vector<SsdRun> runs;
    std::string runsWhy;
    bool runsOk = false;
    if (wantRuns) {
        std::string rtext, rerror;
        runsOk = device_->runs(rtext, &rerror) && parseSsdRuns(rtext, runs, &rerror);
        if (!runsOk) runsWhy = "SSD run table unreadable: " + rerror;
    }
    uint32_t persistId = 0;
    std::string stopWhy;
    {
        std::lock_guard<std::mutex> sm(stopMutex_);
        stopWhy = stopWhy_;
    }
    std::unique_lock<std::mutex> lock(m_);
    status_ = std::move(s);
    if (openedRunId_ != 0 && stopFailed_) {
        // Every stop attempt failed: the drain was stopped first (prestop), so the gate is open and pzrec can be read. It decides whether the run is closed.
        if (ok && !status_.openRun) {
            openedRunId_ = 0;
            stopFailed_ = false;
        } else if (ok) {
            status_.reason = "SSD run " + std::to_string(openedRunId_) + " was not confirmed closed: " + (stopWhy.empty() ? std::string("pzrec stop did not finish") : stopWhy);
        }
    }
    haveStatus_ = true;
    statusAt_ = std::chrono::steady_clock::now();
    if (!ok || status_.state == SsdState::Unformatted || status_.state == SsdState::Initialising || status_.state == SsdState::Wedged ||
        status_.state == SsdState::Absent) {
        runs_.clear();
        haveRuns_ = false;
        runsWhy_ = status_.reason;
    } else if (wantRuns) {
        ticksSinceRuns_ = 0;
        runsSignature_ = signatureOf(status_);
        if (runsOk) {
            if (!baselineLoaded_) {
                baselineLoaded_ = true;
                seenBaseline_ = seenPersisted_ = readSeenId(stateFile_);
            }
            // The recovery is whatever the table says: reason 7 entries above what Studio had seen before this start.
            noticeIds_.clear();
            uint32_t maxId = 0;
            for (const auto& r : runs) {
                maxId = std::max(maxId, r.id);
                if (r.reason == 7 && r.id > seenBaseline_) noticeIds_.push_back(r.id);
            }
            if (maxId > seenPersisted_) {
                seenPersisted_ = maxId;
                persistId = maxId;
            }
            runs_ = std::move(runs);
            haveRuns_ = true;
            runsWhy_.clear();
        } else {
            runs_.clear();
            haveRuns_ = false;
            runsWhy_ = runsWhy;
        }
    }
    status_.recoveredRuns = static_cast<uint32_t>(noticeIds_.size());
    status_.recoveredIds = noticeIds_;
    lock.unlock();
    if (persistId != 0) writeSeenId(stateFile_, persistId);
}

SsdStatus SsdStore::status() {
    if (!background_) refresh(false);
    std::lock_guard<std::mutex> lock(m_);
    return statusLocked();
}

bool SsdStore::runs(std::vector<SsdRun>& out, std::string* why) {
    out.clear();
    if (!background_) refresh(false);
    std::lock_guard<std::mutex> lock(m_);
    if (!device_) { if (why) *why = reasonFor(SsdState::Absent); return false; }
    if (!haveRuns_) {
        if (why) *why = runsWhy_;
        return false;
    }
    out = runs_;
    return true;
}

// ---- Record (#667 S2) ----------------------------------------------------------------------------------------------------------------------------

bool SsdStore::parseFilter(const char* text, SsdFilter& out, std::string* problem) {
    const std::string t = text ? text : "";
    if (t.empty() || t == "valid") { out = SsdFilter::ValidOnly; return true; }
    if (t == "all") { out = SsdFilter::All; return true; }
    if (t == "any") { out = SsdFilter::AnyResult; return true; }
    if (problem) *problem = "MIB_SSD_FILTER=" + t + " is not all, any or valid";
    return false;
}

void SsdStore::setFilterFromEnv(const char* text) {
    configProblem_.clear();
    if (!parseFilter(text, filter_, &configProblem_)) filter_ = SsdFilter::ValidOnly;
}

SsdStore::Prepared SsdStore::prepareRun() {
    Prepared p;
    if (!device_) {
        p.why = reasonFor(SsdState::Absent);
        return p;
    }
    if (!configProblem_.empty()) {
        p.why = configProblem_;
        return p;
    }
    if (stopRunning_) {
        p.state = SsdState::Stopping;
        p.why = "the previous SSD run is still being stopped";
        return p;
    }
    if (const uint32_t open = openedRunId(); open != 0) {
        p.state = SsdState::Recording;
        if (stopFailed_) {
            // A run this Studio opened was never confirmed closed (its stop failed every attempt): try again now instead of leaving the SSD stuck.
            beginStop(false);
            p.state = SsdState::Stopping;
            p.why = "the previous SSD run " + std::to_string(open) + " was not confirmed closed: closing it now, try again in a moment";
        } else {
            p.why = "SSD run " + std::to_string(open) + " is open (RECORDING)";
        }
        return p;
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        if (exportHeldLocked()) {
            p.why = "export in progress: a download of run " + std::to_string(exportRun_) + " is reading the SSD";
            return p;
        }
    }
    refresh(true);      // a fresh answer under the device's short timeout: the cached state may be a second old
    const SsdStatus st = status();
    p.state = st.state;
    if (st.state != SsdState::Ready || st.openRun) {
        p.why = std::string("the SSD is ") + ssdStateName(st.state) + (st.openRun ? " with an open run" : "") +
                (st.reason.empty() ? std::string() : ": " + st.reason);
        return p;
    }
    if (st.nextRunId == 0) {
        p.why = "the SSD reports no next run id";
        return p;
    }
    {
        // The check and the marker are one step against exportBegin(): either the lease is taken first (refused above) or the marker is set first (the export refuses).
        std::lock_guard<std::mutex> lock(m_);
        if (exportHeldLocked()) {
            p.why = "export in progress: a download of run " + std::to_string(exportRun_) + " is reading the SSD";
            return p;
        }
        prepared_ = true;
        preparedAt_ = std::chrono::steady_clock::now();
    }
    p.ok = true;
    p.runId = st.nextRunId;
    return p;
}

void SsdStore::cancelPrepare() {
    std::lock_guard<std::mutex> lock(m_);
    prepared_ = false;
}

bool SsdStore::startRun(uint32_t expectedId, const SsdStartArgs& args, std::string* why, bool* opened) {
    if (opened) *opened = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        prepared_ = false;      // from here the open run (or its refusal) speaks for itself
    }
    auto fail = [&](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    if (!device_) return fail(reasonFor(SsdState::Absent));
    std::lock_guard<std::mutex> device(deviceMutex_);
    std::string text, error;
    if (!device_->start(args, text, &error)) {
        // A run found open after a failed start is assumed to be Studio's own: nothing else starts runs on this unit (the unit is the only caller).
        // A refusal opens nothing, but a start that was killed on its timeout may have written the entry and sent START: ask what is open now, and when even
        // that cannot be read after a timeout, assume it is (the caller aborts; an abort of nothing fails harmlessly).
        std::string stext, serror;
        SsdStatus st;
        const bool known = device_->status(stext, &serror) && parseSsdStatus(stext, st, &serror);
        const bool timedOut = error.find("did not answer") != std::string::npos;
        const bool open = known ? st.openRun : timedOut;
        if (open) {
            {
                std::lock_guard<std::mutex> lock(m_);
                openedRunId_ = known && st.openRunId ? st.openRunId : expectedId;
            }
            if (opened) *opened = true;
        }
        return fail("pzrec start failed: " + error);
    }
    SsdStatus st;
    std::string perror;
    if (!parseSsdStatus(text, st, &perror)) {
        // The CLI exited 0 but its answer does not parse: a run may be open; the caller must stop it.
        std::lock_guard<std::mutex> lock(m_);
        openedRunId_ = expectedId;
        if (opened) *opened = true;
        return fail("pzrec start answered with unreadable status: " + perror);
    }
    if (!st.openRun) return fail(std::string("pzrec start returned without an open run (state ") + ssdStateName(st.state) + ")");
    {
        std::lock_guard<std::mutex> lock(m_);
        openedRunId_ = st.openRunId;
        st.reason = reasonFor(st.state);
        status_ = st;
        haveStatus_ = true;
        statusAt_ = std::chrono::steady_clock::now();
    }
    if (st.openRunId != expectedId) {
        if (opened) *opened = true;
        return fail("run id mismatch: pzrec opened run " + std::to_string(st.openRunId) + ", expected " + std::to_string(expectedId));
    }
    return true;
}

bool SsdStore::stopping() const { return stopRunning_; }

uint32_t SsdStore::openedRunId() const {
    std::lock_guard<std::mutex> lock(m_);
    return openedRunId_;
}

void SsdStore::beginStop(bool abort) {
    if (!device_) return;
    std::lock_guard<std::mutex> lock(stopMutex_);
    if (stopRunning_) return;
    if (stopThread_.joinable()) stopThread_.join();     // a finished earlier attempt
    stopRunning_ = true;
    stopFailed_ = false;
    stopDone_ = false;
    stopOk_ = false;
    stopWhy_.clear();
    stopThread_ = std::thread([this, abort] { stopWorker(abort); });
}

void SsdStore::stopWorker(bool abort) {
    bool ok = false;
    std::string why;
    for (int attempt = 1; attempt <= stopAttempts_ && !shuttingDown_ && !ok; ++attempt) {
        {
            std::lock_guard<std::mutex> device(deviceMutex_);
            std::string text, error;
            if (device_->stop(abort, text, &error)) {
                ok = true;
            } else {
                why = error;
                // A killed or doubted attempt may have closed the run after all (or the drain had ended on its own): trust pzrec's own answer.
                std::string stext, serror;
                SsdStatus st;
                if (device_->status(stext, &serror) && parseSsdStatus(stext, st, &serror) && !st.openRun) ok = true;
            }
        }
        if (!ok && attempt < stopAttempts_) {
            std::unique_lock<std::mutex> lock(stopMutex_);
            stopCv_.wait_for(lock, stopBackoff_, [this] { return shuttingDown_.load(); });
        }
    }
    if (ok) {
        {
            std::lock_guard<std::mutex> lock(m_);
            openedRunId_ = 0;
        }
        refresh(true);
    }
    std::lock_guard<std::mutex> lock(stopMutex_);
    stopOk_ = ok;
    stopFailed_ = !ok;
    stopWhy_ = ok ? std::string() : (why.empty() ? std::string("pzrec stop did not finish") : why);
    stopDone_ = true;
    stopRunning_ = false;
    stopCv_.notify_all();
}

bool SsdStore::waitStopped(std::chrono::milliseconds deadline, std::string* why, bool* stillRunning) {
    if (stillRunning) *stillRunning = false;
    std::unique_lock<std::mutex> lock(stopMutex_);
    if (!stopRunning_ && !stopDone_) {
        // nothing was started
        const uint32_t open = openedRunId();
        if (open != 0 && why) *why = "no stop is in progress for run " + std::to_string(open);
        return open == 0;
    }
    if (!stopCv_.wait_for(lock, deadline, [this] { return stopDone_; })) {
        if (stillRunning) *stillRunning = true;
        if (why) *why = "the SSD run is still stopping";
        return false;
    }
    if (!stopOk_ && why) *why = stopWhy_;
    return stopOk_;
}

// ---- Export (#667) -------------------------------------------------------------------------------------------------------------------------------

bool SsdStore::exportHeldLocked() {
    if (exportLease_ == 0) return false;
    if (std::chrono::steady_clock::now() > exportDeadline_) {
        SPDLOG_WARN("SsdStore: export lease {} for run {} expired after its bound plus the kill margin without exportEnd (the caller is gone; its reader has been terminated and killed by then): released", exportLease_, exportRun_);
        exportLease_ = 0;
        return false;
    }
    return true;
}

bool SsdStore::exportActive() {
    std::lock_guard<std::mutex> lock(m_);
    return exportHeldLocked();
}

namespace {
// How long one download may take: two bounce-path commands (about 25 s each, the bound of one block command) and a margin, plus the body at a floor rate
// of 5 MB/s (the board owner measures the bounce path at about 27 MB/s). The caller terminates its reader at this bound, independently of whether the client reads; the lease itself
// lasts kExportKillMarginSeconds longer (SIGTERM grace, SIGKILL wait).
uint32_t exportBoundSeconds(uint64_t bytes) {
    const uint64_t s = 2 * 25 + 35 + (bytes + 4'999'999) / 5'000'000;
    return static_cast<uint32_t>(std::min<uint64_t>(s, 24 * 3600));
}
} // namespace

// The pzrec calls of exportBegin (window, status, runs: each up to the device timeout, plus a wait for deviceMutex_) run while the caller holds the bridge mutex (mib-app-commands),
// as prepareRun's do: other bridge commands wait that long (milliseconds when pzrec answers, a few seconds when it does not).
SsdStore::ExportBegin SsdStore::exportBegin(uint32_t run, uint64_t from, uint64_t count) {
    ExportBegin out;
    out.runId = run;
    auto refuse = [&](const char* code, const std::string& why) {
        out.ok = false;
        out.code = code;
        out.reason = why;
        SPDLOG_INFO("SsdStore: export of run {} refused: {}", run, why);
        return out;
    };
    if (!device_) return refuse("UNAVAILABLE", reasonFor(SsdState::Absent));
    if (!configProblem_.empty()) return refuse("UNAVAILABLE", configProblem_);
    // The lease first (so a Start that comes now is refused), under the same lock as the state checks: this is the atomic step against prepareRun/startRun/stop.
    uint64_t lease = 0;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (exportHeldLocked()) return refuse("BUSY", "another export is in progress (run " + std::to_string(exportRun_) + ")");
        if (stopRunning_) return refuse("BUSY", "the SSD run is still being stopped: wait for it to finish");
        if (openedRunId_ != 0)
            return refuse("BUSY", stopFailed_ ? "SSD run " + std::to_string(openedRunId_) + " was not confirmed closed after its stop: download only when it is"
                                              : "Studio is recording (SSD run " + std::to_string(openedRunId_) + "): download only when idle");
        if (prepared_ && std::chrono::steady_clock::now() - preparedAt_ < std::chrono::seconds(120)) return refuse("BUSY", "an experiment is starting: download only when idle");
        prepared_ = false;
        lease = nextLease_++;
        exportLease_ = lease;
        exportRun_ = run;
        exportBytes_ = 0;
        exportDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(120);      // provisional, until the size is known
    }
    auto release = [&](const char* code, const std::string& why) {
        std::lock_guard<std::mutex> lock(m_);
        if (exportLease_ == lease) exportLease_ = 0;
        out.ok = false;
        out.code = code;
        out.reason = why;
        SPDLOG_INFO("SsdStore: export of run {} refused: {}", run, why);
        return out;
    };
    std::lock_guard<std::mutex> device(deviceMutex_);      // waits for a refresh already in flight; none starts while the lease is held
    // 1. The window must show no run (registers only, no disk): a run Studio did not start, an armed drain, a stop still draining.
    {
        std::string text, error;
        if (!device_->windowSnapshot(text, &error)) return release("UNAVAILABLE", "cannot verify that no run is active: " + error);
        const json j = json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("state") || !j["state"].is_string()) return release("UNAVAILABLE", "the window snapshot is unreadable: cannot verify that no run is active");
        const std::string state = j["state"].get<std::string>();
        if (state != "IDLE") return release("BUSY", "the SSD drain is " + state + " (a run is active or not yet closed): download only when idle");
    }
    // 2. The run table, fresh (the disk is readable now): the record count the body will hold.
    SsdStatus st;
    std::vector<SsdRun> runs;
    {
        std::string text, error;
        if (!device_->status(text, &error) || !parseSsdStatus(text, st, &error)) return release("UNAVAILABLE", "SSD not responding: " + error);
        if (st.state != SsdState::Ready || st.openRun) return release("UNAVAILABLE", std::string("the SSD is ") + ssdStateName(st.state) + (st.openRun ? " with an open run" : "") + ": download only when READY");
        std::string rtext, rerror;
        if (!device_->runs(rtext, &rerror) || !parseSsdRuns(rtext, runs, &rerror)) return release("UNAVAILABLE", "SSD run table unreadable: " + rerror);
    }
    const SsdRun* found = nullptr;
    for (const auto& r : runs) if (r.id == run) found = &r;
    if (!found) return release("NO_SUCH_RUN", "no such run " + std::to_string(run));
    if (found->deleted) return release("RUN_DELETED", "run " + std::to_string(run) + " was deleted");
    if (found->open) return release("NOT_OFFERED", "run " + std::to_string(run) + " is still open");
    if (found->countsUnknown || found->totalsInconsistent) return release("NOT_OFFERED", "run " + std::to_string(run) + " has no reliable record count (recovered at mount or inconsistent totals): not offered");
    if (found->recSectors * 512 != kRecordBytes) return release("NOT_OFFERED", "run " + std::to_string(run) + " has an unexpected record size (" + std::to_string(found->recSectors * 512) + " B)");
    if (found->written == 0) return release("NOT_OFFERED", "run " + std::to_string(run) + " holds no records");
    if (from >= found->written) return release("BAD_RANGE", "--from " + std::to_string(from) + " is outside the run (" + std::to_string(found->written) + " records)");
    const uint64_t records = count != 0 ? count : found->written - from;
    if (records > found->written - from) return release("BAD_RANGE", "--from/--count do not lie inside the run (" + std::to_string(found->written) + " records): refused, never clipped");
    std::vector<std::string> argv;
    if (!device_->readArgv(run, from, count, argv)) return release("UNAVAILABLE", "this SSD device cannot read a run");
    // 3. The lease is real now: its bound follows the size.
    {
        std::lock_guard<std::mutex> lock(m_);
        if (exportLease_ != lease) return release("BUSY", "the export lease was lost");     // expired meanwhile: not given
        exportBytes_ = records * kRecordBytes;
        out.maxSeconds = exportMaxSecondsOverride_ ? exportMaxSecondsOverride_ : exportBoundSeconds(exportBytes_);
        exportDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(out.maxSeconds + exportMarginSeconds_);
    }
    out.ok = true;
    out.lease = lease;
    out.records = records;
    out.bytes = records * kRecordBytes;
    out.argv = std::move(argv);
    SPDLOG_INFO("SsdStore: export lease {} taken for run {}: {} records, {} bytes, bound {} s", lease, run, records, out.bytes, out.maxSeconds);
    return out;
}

void SsdStore::exportEnd(uint64_t lease, uint64_t bytesSent, const std::string& outcome) {
    std::lock_guard<std::mutex> lock(m_);
    if (lease == 0 || exportLease_ != lease) {
        SPDLOG_DEBUG("SsdStore: export end for lease {} ({}): not held (already released or expired)", lease, outcome);
        return;
    }
    SPDLOG_INFO("SsdStore: export lease {} released for run {}: {} of {} bytes sent, outcome: {}", lease, exportRun_, bytesSent, exportBytes_, outcome);
    exportLease_ = 0;
}

} // namespace backend::pz
