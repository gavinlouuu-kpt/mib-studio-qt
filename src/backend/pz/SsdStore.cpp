#include "backend/pz/SsdStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
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
        runs.push_back(r);
    }
    out = std::move(runs);
    return true;
}

RunResult runBounded(const std::vector<std::string>& argv, std::chrono::milliseconds timeout) {
    RunResult r;
#if defined(_WIN32)
    (void)argv; (void)timeout;
    r.err = "pzrec is not available on this platform";
    return r;
#else
    if (argv.empty()) { r.err = "no command"; return r; }
    int outPipe[2], errPipe[2];
    if (pipe2(outPipe, O_CLOEXEC) != 0) { r.err = std::string("pipe: ") + std::strerror(errno); return r; }
    if (pipe2(errPipe, O_CLOEXEC) != 0) {
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
    constexpr size_t kMaxCapture = 4u << 20;  // a run table of 256 entries is about 60 KB
    while (open[0] || open[1]) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) { r.timedOut = true; break; }
        pollfd p[2];
        int n = 0, idx[2];
        for (int i = 0; i < 2; ++i) if (open[i]) { p[n] = {fds[i], POLLIN, 0}; idx[n++] = i; }
        const int pr = poll(p, n, static_cast<int>(std::min<long long>(left.count(), 1000)));
        if (pr < 0) { if (errno == EINTR) continue; break; }
        for (int k = 0; k < n; ++k) {
            if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[4096];
            const ssize_t got = read(fds[idx[k]], buf, sizeof buf);
            if (got > 0) {
                std::string& dst = idx[k] == 0 ? r.out : r.err;
                if (dst.size() < kMaxCapture) dst.append(buf, static_cast<size_t>(got));
            } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
                open[idx[k]] = false;
            }
        }
    }
    close(outPipe[0]);
    close(errPipe[0]);
    if (r.timedOut) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!r.timedOut) r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
#endif
}

PzrecCliDevice::PzrecCliDevice(std::string pzrecPath, std::string imagePath, std::chrono::milliseconds timeout)
    : pzrec_(std::move(pzrecPath)), image_(std::move(imagePath)), timeout_(timeout) {}

bool PzrecCliDevice::call(const char* verb, std::string& json, std::string* error) {
    const RunResult r = runBounded({pzrec_, verb, image_}, timeout_);
    auto firstLine = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.size() > 300) s.resize(300);
        return s;
    };
    if (!r.started) { if (error) *error = "pzrec could not be run: " + firstLine(r.err); return false; }
    if (r.timedOut) { if (error) *error = "pzrec " + std::string(verb) + " did not answer within " + std::to_string(timeout_.count()) + " ms"; return false; }
    if (r.exitCode != 0) {
        if (error) {
            *error = "pzrec " + std::string(verb) + " exited with " + std::to_string(r.exitCode);
            const std::string detail = firstLine(r.err.empty() ? r.out : r.err);
            if (!detail.empty()) *error += ": " + detail;
        }
        return false;
    }
    json = r.out;
    return true;
}

bool PzrecCliDevice::status(std::string& json, std::string* error) { return call("status", json, error); }
bool PzrecCliDevice::runs(std::string& json, std::string* error) { return call("runs", json, error); }

SsdStore::SsdStore(std::unique_ptr<ISsdDevice> device, std::chrono::milliseconds interval, bool background)
    : device_(std::move(device)), interval_(interval), background_(background && device_ != nullptr) {
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
    return status_;
}

void SsdStore::refresh(bool force) {
    if (!device_) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!force && haveStatus_ && std::chrono::steady_clock::now() - statusAt_ < interval_) return;
    }
    // The pzrec calls run without the lock held: a slow child never blocks status() readers.
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
    std::lock_guard<std::mutex> lock(m_);
    if (ok) {
        std::vector<uint32_t> fresh;
        for (uint32_t id : s.recoveredIds)
            if (std::find(latchedRecoveredIds_.begin(), latchedRecoveredIds_.end(), id) == latchedRecoveredIds_.end()) fresh.push_back(id);
        if (!fresh.empty()) {
            latchedRecovered_ += static_cast<uint32_t>(fresh.size());
            latchedRecoveredIds_.insert(latchedRecoveredIds_.end(), fresh.begin(), fresh.end());
        } else if (s.recoveredIds.empty() && s.recoveredRuns > 0) {
            latchedRecovered_ += s.recoveredRuns;
        }
        latchedSkipped_ = std::max(latchedSkipped_, s.skippedBadEntries);
        s.recoveredRuns = latchedRecovered_;
        s.recoveredIds = latchedRecoveredIds_;
        s.skippedBadEntries = latchedSkipped_;
    }
    status_ = std::move(s);
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
            runs_ = std::move(runs);
            haveRuns_ = true;
            runsWhy_.clear();
        } else {
            runs_.clear();
            haveRuns_ = false;
            runsWhy_ = runsWhy;
        }
    }
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

} // namespace backend::pz
