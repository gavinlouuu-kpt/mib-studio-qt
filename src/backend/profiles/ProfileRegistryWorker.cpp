#include "backend/profiles/ProfileRegistryWorker.h"
#include "backend/processing/ProcessingCoreSha256.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

namespace backend::profiles {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kRetainedJobs = 64;
constexpr std::size_t kPublishEveryPages = 25;
constexpr std::size_t kMaxRevisionIdBytes = 128;
constexpr const char* kLastSessionFile = "last_session.json";

constexpr const char* kMethodConfigFile = "config.json";
constexpr const char* kMethodCameraFile = "egrabberConfig.js";
constexpr const char* kMethodCanonicalFile = "method.canonical.json";

void wipe(std::string& secret) {
    std::fill(secret.begin(), secret.end(), '\0');
    secret.clear();
}

// Revision IDs come from the server; only a plain token may name a directory.
bool safePathToken(const std::string& id) {
    if (id.empty() || id.size() > kMaxRevisionIdBytes || id.front() == '.') return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// The exact files a revision materializes to.
std::map<std::string, std::string> methodFiles(const Revision& revision) {
    const auto envelope = Json::parse(revision.canonicalContent);
    return {{kMethodConfigFile, envelope.at("config").dump(4) + "\n"},
            {kMethodCameraFile, envelope.at("camera_script").get<std::string>()},
            {kMethodCanonicalFile, revision.canonicalContent}};
}

bool filesMatch(const std::filesystem::path& dir, const std::map<std::string, std::string>& files) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (const auto& [name, bytes] : files)
        if (readFile(dir / name) != bytes) return false;
    return true;
}

void makeWritable(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, ec);
}

std::string utcNowIso8601() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return out;
}
} // namespace

const char* toString(RegistryJobKind kind) {
    switch (kind) {
    case RegistryJobKind::SignIn:
        return "sign_in";
    case RegistryJobKind::SignOut:
        return "sign_out";
    case RegistryJobKind::Refresh:
        return "refresh";
    case RegistryJobKind::Download:
        return "download";
    case RegistryJobKind::Materialize:
        return "materialize";
    case RegistryJobKind::RecordValidation:
        return "record_validation";
    }
    return "unknown";
}

const char* toString(RegistryJobState state) {
    switch (state) {
    case RegistryJobState::Queued:
        return "queued";
    case RegistryJobState::Running:
        return "running";
    case RegistryJobState::Succeeded:
        return "succeeded";
    case RegistryJobState::Partial:
        return "partial";
    case RegistryJobState::Failed:
        return "failed";
    case RegistryJobState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

const char* toString(RegistryWorkerSnapshot::Session session) {
    switch (session) {
    case RegistryWorkerSnapshot::Session::SignedOut:
        return "signed_out";
    case RegistryWorkerSnapshot::Session::SignedIn:
        return "signed_in";
    case RegistryWorkerSnapshot::Session::CachedOffline:
        return "cached_offline";
    }
    return "unknown";
}

struct ProfileRegistryWorker::Active {
    std::unique_ptr<ProfileCache> cache;
    std::unique_ptr<SupabaseProfileRegistry> registry;
    std::unique_ptr<ProfileRegistryService> service;
};

ProfileRegistryWorker::ProfileRegistryWorker(RegistryWorkerConfig config,
                                             RegistryHttpTransport transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    published_.origin = config_.origin;
    if (!config_.configured()) return; // inert: no thread, every request refused
    try {
        if (!transport_)
            throw RegistryError(RegistryErrorCode::Invalid, "No registry HTTP transport supplied");
        if (config_.cacheDir.empty())
            throw RegistryError(RegistryErrorCode::Invalid, "No registry cache directory");
        auth_ = std::make_unique<SupabaseAuth>(config_.origin, config_.publishableKey, transport_);
        auth_->setCancellation([this] { return cancelRequested(); });
    } catch (const RegistryError& e) {
        SPDLOG_WARN("ProfileRegistry: disabled: {}", e.what());
        published_.health.connectivity = RegistryHealth::Connectivity::Failed;
        published_.health.message = e.what();
        return;
    }
    usable_ = true;
    loading_ = true;
    published_.configured = true;
    thread_ = std::thread([this] { run(); });
}

ProfileRegistryWorker::~ProfileRegistryWorker() {
    shutdown();
}

void ProfileRegistryWorker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) {
            stopping_ = true;
            stopRequested_ = true;
            for (auto& command : queue_) {
                wipe(command.secret);
                setJobLocked({command.id, command.kind, RegistryJobState::Cancelled,
                              "Registry worker shut down"});
            }
            queue_.clear();
        }
    }
    cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

std::uint64_t ProfileRegistryWorker::enqueue(Command command) {
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!usable_ || stopping_) return 0;
        if (command.kind == RegistryJobKind::Refresh) {
            for (const auto& queued : queue_)
                if (queued.kind == RegistryJobKind::Refresh) return queued.id;
        }
        id = command.id = nextJobId_++;
        setJobLocked({id, command.kind, RegistryJobState::Queued, {}});
        queue_.push_back(std::move(command));
    }
    cv_.notify_all();
    return id;
}

std::uint64_t ProfileRegistryWorker::requestSignIn(std::string email, std::string password) {
    if (email.empty() || password.empty()) {
        wipe(password);
        return 0;
    }
    Command command;
    command.kind = RegistryJobKind::SignIn;
    command.argument = std::move(email);
    command.secret = std::move(password);
    const auto id = enqueue(std::move(command));
    return id;
}

std::uint64_t ProfileRegistryWorker::requestSignOut() {
    Command command;
    command.kind = RegistryJobKind::SignOut;
    return enqueue(std::move(command));
}

std::uint64_t ProfileRegistryWorker::requestRefresh() {
    Command command;
    command.kind = RegistryJobKind::Refresh;
    return enqueue(std::move(command));
}

std::uint64_t ProfileRegistryWorker::requestDownload(std::string revisionId) {
    if (revisionId.empty() || revisionId.size() > kMaxRevisionIdBytes) return 0;
    Command command;
    command.kind = RegistryJobKind::Download;
    command.argument = std::move(revisionId);
    return enqueue(std::move(command));
}

std::uint64_t ProfileRegistryWorker::requestMaterialize(std::string revisionId) {
    if (!safePathToken(revisionId) || config_.methodsDir.empty()) return 0;
    Command command;
    command.kind = RegistryJobKind::Materialize;
    command.argument = std::move(revisionId);
    return enqueue(std::move(command));
}

std::uint64_t ProfileRegistryWorker::requestRecordValidation(LocalValidationRequest request) {
    if (request.revisionId.empty() || request.revisionId.size() > kMaxRevisionIdBytes ||
        request.context.instrumentId.empty() || request.evidenceFile.empty())
        return 0;
    Command command;
    command.kind = RegistryJobKind::RecordValidation;
    command.argument = request.revisionId;
    command.validation = std::move(request);
    return enqueue(std::move(command));
}

void ProfileRegistryWorker::cancelAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& command : queue_) {
            wipe(command.secret);
            setJobLocked({command.id, command.kind, RegistryJobState::Cancelled, "Cancelled"});
        }
        queue_.clear();
        if (runningJob_ != 0) cancelRunning_ = true;
    }
    cv_.notify_all();
}

RegistryWorkerSnapshot ProfileRegistryWorker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto out = published_;
    out.queuedJobs = queue_.size();
    out.busy = runningJob_ != 0 || loading_;
    return out;
}

std::uint64_t ProfileRegistryWorker::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_.generation;
}

RegistryJobStatus ProfileRegistryWorker::job(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? RegistryJobStatus{} : it->second;
}

bool ProfileRegistryWorker::waitForJob(std::uint64_t id, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] {
        const auto it = jobs_.find(id);
        return it == jobs_.end() || it->second.terminal();
    });
}

bool ProfileRegistryWorker::waitIdle(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [&] { return queue_.empty() && runningJob_ == 0 && !loading_; });
}

void ProfileRegistryWorker::setJobLocked(const RegistryJobStatus& status) {
    jobs_[status.id] = status;
    if (status.terminal()) published_.lastJob = status;
    while (jobs_.size() > kRetainedJobs) {
        auto oldest = std::find_if(jobs_.begin(), jobs_.end(),
                                   [](const auto& entry) { return entry.second.terminal(); });
        if (oldest == jobs_.end()) break;
        jobs_.erase(oldest);
    }
    ++published_.generation;
}

bool ProfileRegistryWorker::cancelRequested() const {
    return cancelRunning_.load() || stopRequested_.load();
}

void ProfileRegistryWorker::run() {
    try {
        loadLastSession();
    } catch (const std::exception& e) {
        cacheError_ = e.what();
    }
    publish();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loading_ = false;
    }
    cv_.notify_all();
    for (;;) {
        Command command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            command = std::move(queue_.front());
            queue_.pop_front();
            runningJob_ = command.id;
            cancelRunning_ = false;
            setJobLocked({command.id, command.kind, RegistryJobState::Running, {}});
        }
        cv_.notify_all();
        RegistryJobStatus status{command.id, command.kind, RegistryJobState::Failed, {}};
        try {
            status = execute(command);
        } catch (const std::exception& e) {
            status.state = RegistryJobState::Failed;
            status.message = e.what();
        }
        wipe(command.secret);
        status.id = command.id;
        status.kind = command.kind;
        if (cancelRequested() && status.state != RegistryJobState::Succeeded) {
            status.state = RegistryJobState::Cancelled;
            status.message = "Cancelled";
        }
        publish();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            setJobLocked(status);
            runningJob_ = 0;
        }
        cv_.notify_all();
    }
    // Shutdown: tokens die with the worker; nothing token-bearing was persisted.
    wipe(session_.accessToken);
    wipe(session_.refreshToken);
}

RegistryJobStatus ProfileRegistryWorker::execute(Command& command) {
    switch (command.kind) {
    case RegistryJobKind::SignIn:
        return doSignIn(command.argument, command.secret);
    case RegistryJobKind::SignOut:
        return doSignOut();
    case RegistryJobKind::Refresh:
        return doRefresh();
    case RegistryJobKind::Download:
        return doDownload(command.argument);
    case RegistryJobKind::Materialize:
        return doMaterialize(command.argument);
    case RegistryJobKind::RecordValidation:
        if (command.validation) return doRecordValidation(*command.validation);
        break;
    }
    return {command.id, command.kind, RegistryJobState::Failed, "Unknown registry command"};
}

void ProfileRegistryWorker::noteSuccess() {
    health_.connectivity = RegistryHealth::Connectivity::Online;
    health_.message.clear();
    ++health_.successfulRequests;
}

void ProfileRegistryWorker::noteFailure(RegistryErrorCode code, const std::string& message) {
    if (cancelRequested()) return; // an aborted request says nothing about the registry
    ++health_.failedRequests;
    health_.message = message;
    switch (code) {
    case RegistryErrorCode::Offline:
        health_.connectivity = RegistryHealth::Connectivity::Offline;
        break;
    case RegistryErrorCode::Authentication:
        health_.connectivity = RegistryHealth::Connectivity::AuthenticationRequired;
        break;
    case RegistryErrorCode::Permission:
        health_.connectivity = RegistryHealth::Connectivity::PermissionDenied;
        break;
    default:
        health_.connectivity = RegistryHealth::Connectivity::Failed;
        break;
    }
}

void ProfileRegistryWorker::noteService(bool ok) {
    if (!active_ || !active_->service || cancelRequested()) return;
    const auto& h = active_->service->health();
    health_.rejectedRevisions = h.rejectedRevisions;
    health_.lastRejection = h.lastRejection;
    if (ok) {
        noteSuccess();
        return;
    }
    ++health_.failedRequests;
    health_.connectivity = h.connectivity;
    health_.message = h.message;
}

bool ProfileRegistryWorker::refreshSession() {
    try {
        auto refreshed = auth_->refresh(session_.refreshToken);
        if (refreshed.userId != subjectId_)
            throw RegistryError(RegistryErrorCode::Authentication,
                                "Registry session changed user; sign in again");
        wipe(session_.accessToken);
        wipe(session_.refreshToken);
        session_ = std::move(refreshed);
        return true;
    } catch (const RegistryError& e) {
        if (e.code == RegistryErrorCode::Authentication && !cancelRequested()) {
            // Refresh token rejected/reused: tokens are gone, the cache stays
            // open read-only for this user (CachedOffline) until a new sign-in.
            wipe(session_.accessToken);
            wipe(session_.refreshToken);
            session_ = {};
            noteFailure(e.code, "Registry session expired; sign in again");
        } else {
            noteFailure(e.code, e.what());
        }
        return false;
    }
}

bool ProfileRegistryWorker::withSession(const std::function<bool()>& op) {
    if (!session_.valid()) {
        noteFailure(RegistryErrorCode::Authentication, "Sign in to reach the central registry");
        return false;
    }
    if (std::chrono::system_clock::now() + config_.tokenRefreshMargin >= session_.expiresAt &&
        !refreshSession())
        return false;
    bool ok = op();
    if (!ok && !cancelRequested() &&
        active_->service->health().connectivity ==
            RegistryHealth::Connectivity::AuthenticationRequired) {
        // Access token rejected before its recorded expiry (revoked, clock
        // skew): rotate once and retry once.
        if (!refreshSession()) return false;
        ok = op();
    }
    noteService(ok);
    return ok;
}

std::filesystem::path ProfileRegistryWorker::cachePathFor(const std::string& subject) const {
    // Scoped by origin + subject (ProfileCache also enforces this inside the file).
    return config_.cacheDir /
           ("cache-" + contentHash(config_.origin + "\n" + subject).substr(0, 24) + ".sqlite");
}

bool ProfileRegistryWorker::openUser(const std::string& subject, const std::string& email,
                                     bool persistLastSession) {
    closeUser();
    std::error_code ec;
    std::filesystem::create_directories(config_.cacheDir, ec);
    try {
        auto active = std::make_unique<Active>();
        active->cache =
            std::make_unique<ProfileCache>(cachePathFor(subject).string(), config_.origin, subject);
        active->registry = std::make_unique<SupabaseProfileRegistry>(
            config_.origin, config_.publishableKey, [this] { return session_.accessToken; },
            transport_);
        active->registry->setCancellation([this] { return cancelRequested(); });
        active->service =
            std::make_unique<ProfileRegistryService>(*active->registry, *active->cache);
        active_ = std::move(active);
    } catch (const RegistryError& e) {
        cacheError_ = std::string("Profile cache unavailable: ") + e.what();
        SPDLOG_WARN("ProfileRegistry: {}", cacheError_);
        return false;
    }
    cacheError_.clear();
    subjectId_ = subject;
    email_ = email;
    scanMaterialized();
    if (persistLastSession) {
        // No tokens: just enough to reopen this user's cache offline.
        const auto path = config_.cacheDir / kLastSessionFile;
        const auto tmp = config_.cacheDir / (std::string(kLastSessionFile) + ".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << Json({{"origin", config_.origin}, {"subject", subject}, {"email", email}})
                       .dump();
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) SPDLOG_WARN("ProfileRegistry: could not record last session: {}", ec.message());
    }
    return true;
}

void ProfileRegistryWorker::closeUser() {
    active_.reset();
    subjectId_.clear();
    email_.clear();
    projects_.clear();
    lastSuccessfulRefresh_.reset();
    materialized_.clear();
}

void ProfileRegistryWorker::scanMaterialized() {
    materialized_.clear();
    if (config_.methodsDir.empty() || !active_) return;
    try {
        for (const auto& r : active_->cache->listAll()) {
            if (!safePathToken(r.revisionId)) continue;
            const auto dir = config_.methodsDir / r.revisionId;
            std::error_code ec;
            if (std::filesystem::is_directory(dir, ec) && filesMatch(dir, methodFiles(r)))
                materialized_[r.revisionId] = dir.string();
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("ProfileRegistry: materialized method scan failed: {}", e.what());
    }
}

void ProfileRegistryWorker::loadLastSession() {
    const auto path = config_.cacheDir / kLastSessionFile;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::stringstream text;
    text << in.rdbuf();
    try {
        const auto j = Json::parse(text.str());
        const auto origin = j.at("origin").get<std::string>();
        const auto subject = j.at("subject").get<std::string>();
        const auto email = j.value("email", std::string{});
        if (origin != config_.origin || subject.empty()) return; // another registry
        openUser(subject, email, false);
    } catch (const Json::exception&) {
        SPDLOG_WARN("ProfileRegistry: ignoring unreadable {}", path.string());
    }
}

RegistryJobStatus ProfileRegistryWorker::doSignIn(const std::string& email, std::string& password) {
    AuthSession signedIn;
    try {
        signedIn = auth_->signInWithPassword(email, password);
    } catch (const RegistryError& e) {
        wipe(password);
        noteFailure(e.code, e.what());
        return {0, RegistryJobKind::SignIn, RegistryJobState::Failed, e.what()};
    }
    wipe(password);
    noteSuccess();
    // Discard the previous user's tokens before opening (or reopening) the
    // signed-in user's cache; this also rewrites last_session.json.
    wipe(session_.accessToken);
    wipe(session_.refreshToken);
    session_ = {};
    if (!openUser(signedIn.userId, signedIn.email, true)) {
        wipe(signedIn.accessToken);
        wipe(signedIn.refreshToken);
        return {0, RegistryJobKind::SignIn, RegistryJobState::Failed, cacheError_};
    }
    session_ = std::move(signedIn);
    SPDLOG_INFO("ProfileRegistry: signed in (subject {})", subjectId_);
    return {0, RegistryJobKind::SignIn, RegistryJobState::Succeeded, "Signed in"};
}

RegistryJobStatus ProfileRegistryWorker::doSignOut() {
    if (session_.valid()) {
        try {
            auth_->signOut(session_.accessToken);
        } catch (const RegistryError& e) {
            // Best effort: local tokens are discarded regardless.
            SPDLOG_INFO("ProfileRegistry: server sign-out not confirmed: {}", e.what());
        }
    }
    wipe(session_.accessToken);
    wipe(session_.refreshToken);
    session_ = {};
    closeUser();
    std::error_code ec;
    std::filesystem::remove(config_.cacheDir / kLastSessionFile, ec);
    health_.connectivity = RegistryHealth::Connectivity::Unknown;
    health_.message = "Signed out";
    return {0, RegistryJobKind::SignOut, RegistryJobState::Succeeded, "Signed out"};
}

RegistryJobStatus ProfileRegistryWorker::doRefresh() {
    const auto fail = [this](const std::string& fallback) {
        return RegistryJobStatus{0, RegistryJobKind::Refresh, RegistryJobState::Failed,
                                 health_.message.empty() ? fallback : health_.message};
    };
    if (!active_) return fail("Sign in to refresh central methods");
    const auto started = std::chrono::steady_clock::now();
    std::optional<std::vector<RegistryProject>> projects;
    if (!withSession([&] {
            projects = active_->service->listProjects();
            return projects.has_value();
        }))
        return fail("Could not list registry projects");
    projects_ = *projects;
    publish();

    std::size_t pages = 0;
    for (const auto& project : projects_) {
        std::string cursor;
        for (;;) {
            if (cancelRequested())
                return {0, RegistryJobKind::Refresh, RegistryJobState::Cancelled, "Cancelled"};
            if (pages >= config_.maxPagesPerRefresh ||
                std::chrono::steady_clock::now() - started >= config_.refreshBudget) {
                return {0, RegistryJobKind::Refresh, RegistryJobState::Partial,
                        "Refresh budget reached after " + std::to_string(pages) +
                            " page(s); cached revisions remain valid, refresh again to continue"};
            }
            std::optional<std::string> next;
            if (!withSession([&] {
                    next = active_->service->syncPage(project.projectId, cursor);
                    return next.has_value();
                }))
                return fail("Registry refresh failed");
            ++pages;
            if (pages % kPublishEveryPages == 0) publish();
            if (next->empty()) break;
            cursor = *next;
        }
    }
    lastSuccessfulRefresh_ = std::chrono::system_clock::now();
    return {0, RegistryJobKind::Refresh, RegistryJobState::Succeeded,
            "Refreshed " + std::to_string(projects_.size()) + " project(s), " +
                std::to_string(pages) + " page(s)"};
}

RegistryJobStatus ProfileRegistryWorker::doDownload(const std::string& revisionId) {
    if (!active_)
        return {0, RegistryJobKind::Download, RegistryJobState::Failed,
                "Sign in to download central methods"};
    if (!withSession([&] { return active_->service->download(revisionId); }))
        return {0, RegistryJobKind::Download, RegistryJobState::Failed, health_.message};
    return {0, RegistryJobKind::Download, RegistryJobState::Succeeded, "Downloaded"};
}

RegistryJobStatus ProfileRegistryWorker::doMaterialize(const std::string& revisionId) {
    const auto fail = [](const std::string& message) {
        return RegistryJobStatus{0, RegistryJobKind::Materialize, RegistryJobState::Failed,
                                 message};
    };
    if (!active_) return fail("No cached methods are open; sign in first");
    Revision revision;
    try {
        revision = active_->cache->read(revisionId); // verified on read
    } catch (const RegistryError& e) {
        return fail(std::string("Revision not available in the cache: ") + e.what());
    }
    std::map<std::string, std::string> files;
    try {
        files = methodFiles(revision);
    } catch (const std::exception& e) {
        return fail(std::string("Revision content unreadable: ") + e.what());
    }
    const auto dir = config_.methodsDir / revisionId;
    if (filesMatch(dir, files)) {
        materialized_[revisionId] = dir.string();
        return {0, RegistryJobKind::Materialize, RegistryJobState::Succeeded,
                "Already materialized at " + dir.string()};
    }
    // Stage the whole directory, then swap it in: a crash leaves either the
    // old verified files or none, never a half-written method.
    const auto staging = config_.methodsDir / (".staging-" + revisionId);
    std::error_code ec;
    makeWritable(staging);
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) return fail("Cannot create " + staging.string() + ": " + ec.message());
    for (const auto& [name, bytes] : files) {
        std::ofstream out(staging / name, std::ios::binary | std::ios::trunc);
        out << bytes;
        out.close();
        if (!out) return fail("Cannot write " + (staging / name).string());
        std::filesystem::permissions(staging / name,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::group_read |
                                         std::filesystem::perms::others_read,
                                     ec);
    }
    makeWritable(dir);
    std::filesystem::remove_all(dir, ec);
    std::filesystem::rename(staging, dir, ec);
    if (ec) return fail("Cannot install " + dir.string() + ": " + ec.message());
    materialized_[revisionId] = dir.string();
    return {0, RegistryJobKind::Materialize, RegistryJobState::Succeeded,
            "Materialized to " + dir.string()};
}

RegistryJobStatus ProfileRegistryWorker::doRecordValidation(const LocalValidationRequest& request) {
    const auto fail = [](const std::string& message) {
        return RegistryJobStatus{0, RegistryJobKind::RecordValidation, RegistryJobState::Failed,
                                 message};
    };
    // "Who" must be an authenticated registry user, not a remembered name.
    if (!session_.valid() || !active_)
        return fail("Sign in to record a local validation; the validator is the signed-in user");
    Revision revision;
    try {
        revision = active_->cache->read(request.revisionId);
    } catch (const RegistryError& e) {
        return fail(std::string("Revision not available in the cache: ") + e.what());
    }
    if (revision.state != CentralState::Published && revision.state != CentralState::Superseded)
        return fail(std::string("Only published or superseded revisions can be validated (this "
                                "one is ") +
                    toString(revision.state) + ")");
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(request.evidenceFile, ec);
    if (ec) return fail("Evidence file not readable: " + request.evidenceFile);
    std::string hashError;
    const auto evidenceSha = processing::fileSha256(request.evidenceFile, &hashError,
                                                    [this] { return cancelRequested(); });
    if (evidenceSha.empty()) {
        if (hashError == "cancelled")
            return {0, RegistryJobKind::RecordValidation, RegistryJobState::Cancelled,
                    "Cancelled"};
        return fail("Evidence file could not be hashed: " + request.evidenceFile);
    }
    LocalValidation validation;
    validation.revisionId = revision.revisionId;
    validation.instrumentId = request.context.instrumentId;
    validation.contentHash = revision.contentHash;
    validation.contextHash = methodContextHash(request.context);
    validation.validatorId = subjectId_;
    validation.evidence =
        Json({{"schema", 1},
              {"run_file", request.evidenceFile},
              {"run_file_sha256", evidenceSha},
              {"run_file_bytes", bytes},
              {"instrument_name", request.instrumentName},
              {"validator_email", email_},
              {"confirmed_at_utc", utcNowIso8601()},
              {"context", Json::parse(methodContextJson(request.context))}})
            .dump(-1, ' ', false, Json::error_handler_t::replace);
    validation.passed = request.passed;
    try {
        active_->cache->recordValidation(validation);
    } catch (const RegistryError& e) {
        return fail(std::string("Validation not recorded: ") + e.what());
    }
    SPDLOG_INFO("ProfileRegistry: local validation of {} recorded ({}) by {}", revision.revisionId,
                request.passed ? "passed" : "failed", subjectId_);
    return {0, RegistryJobKind::RecordValidation, RegistryJobState::Succeeded,
            request.passed ? "Local validation recorded" : "Failed validation recorded"};
}

void ProfileRegistryWorker::publish() {
    // Read the cache on the worker thread, outside the lock: snapshot() callers
    // (UI thread) never wait on SQLite or hashing.
    std::vector<CachedRevisionSummary> revisions;
    std::vector<LocalValidationRecord> validations;
    std::vector<std::string> corrupt;
    std::string cacheError = cacheError_;
    if (active_) {
        try {
            for (const auto& r : active_->cache->listAll(&corrupt)) {
                const auto materialized = materialized_.find(r.revisionId);
                revisions.push_back({r.revisionId, r.methodId, r.projectId, r.displayName,
                                     r.authorId, r.contentHash, r.revisionNumber, r.metadataVersion,
                                     r.state, revisionConfigSha256(r.canonicalContent),
                                     materialized == materialized_.end() ? std::string{}
                                                                         : materialized->second});
            }
            validations = active_->cache->listValidations();
        } catch (const std::exception& e) {
            // publish() runs outside run()'s job try-block: nothing may escape
            // the worker thread.
            revisions.clear();
            validations.clear();
            cacheError = std::string("Profile cache unreadable: ") + e.what();
        }
    }
    using Session = RegistryWorkerSnapshot::Session;
    std::lock_guard<std::mutex> lock(mutex_);
    published_.session = session_.valid() ? Session::SignedIn
                         : active_        ? Session::CachedOffline
                                          : Session::SignedOut;
    published_.subjectId = subjectId_;
    published_.email = email_;
    published_.health = health_;
    published_.projects = projects_;
    published_.revisions = std::move(revisions);
    published_.validations = std::move(validations);
    published_.corruptRevisionIds = std::move(corrupt);
    published_.cacheError = std::move(cacheError);
    published_.lastSuccessfulRefresh = lastSuccessfulRefresh_;
    ++published_.generation;
}

} // namespace backend::profiles
