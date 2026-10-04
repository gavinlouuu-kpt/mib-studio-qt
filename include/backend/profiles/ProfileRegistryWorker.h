#pragma once
// Backend-owned central profile registry worker (#398 M1/M2).
//
// One thread owns sign-in, the Supabase provider, the per-user SQLite cache and
// every network request. Shells enqueue commands and read value snapshots; no
// call here blocks on the network, and nothing here applies or starts
// anything. Materialize only writes a cached revision's files (read-only) under
// methodsDir for a shell to apply; RecordValidation stores the operator's
// local validation in the cache. The registry is never on the acquisition/recording/Start path: an
// outage only changes this worker's snapshot (`health`), never instrument state.
//
// Threading: request*/cancelAll/snapshot/job are callable from any thread and
// never wait for a request in flight. shutdown() cancels the running request
// (through RegistryHttpRequest::cancelled), drops queued work and joins.
//
// Credentials: the password lives only in the queued command until the worker
// takes it; tokens only in worker memory. Nothing token-bearing is persisted:
// `last_session.json` records origin/subject/email so the last user's cached
// revisions stay listable offline after a restart. Explicit sign-out removes
// it and closes that cache (shared-instrument hygiene; the file stays for the
// user's next sign-in). Vault: knowledge_map/services/ProfileRegistryService.md.
#include "backend/profiles/ProfileCache.h"
#include "backend/profiles/ProfileRegistryService.h"
#include "backend/profiles/SupabaseAuth.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace backend::profiles {

struct RegistryWorkerConfig {
    std::string origin;         // https://<project>.supabase.co
    std::string publishableKey; // sb_publishable_... (never a service-role key)
    std::filesystem::path cacheDir;
    // Materialized revisions: <methodsDir>/<revisionId>/{config.json,
    // egrabberConfig.js, method.canonical.json}, read-only. Empty disables
    // Materialize.
    std::filesystem::path methodsDir;
    // A refresh scans every member project from the first page; these bound one
    // refresh job. Hitting either ends the job as Partial (cache stays valid).
    std::size_t maxPagesPerRefresh{2000};
    std::chrono::milliseconds refreshBudget{std::chrono::minutes(2)};
    // Refresh the access token this long before it expires.
    std::chrono::seconds tokenRefreshMargin{60};
    bool configured() const { return !origin.empty() && !publishableKey.empty(); }
};

// Contract-pinned (bridge-contract.json registry_job_kinds); append only.
enum class RegistryJobKind { SignIn, SignOut, Refresh, Download, Materialize, RecordValidation };
enum class RegistryJobState { Queued, Running, Succeeded, Partial, Failed, Cancelled };
const char* toString(RegistryJobKind kind);
const char* toString(RegistryJobState state);

struct RegistryJobStatus {
    std::uint64_t id{0}; // 0: unknown, evicted, or the request was refused
    RegistryJobKind kind{RegistryJobKind::Refresh};
    RegistryJobState state{RegistryJobState::Failed};
    std::string message;
    bool terminal() const {
        return state != RegistryJobState::Queued && state != RegistryJobState::Running;
    }
};

struct CachedRevisionSummary {
    std::string revisionId;
    std::string methodId;
    std::string projectId;
    std::string displayName;
    std::string authorId;
    std::string contentHash;
    std::uint64_t revisionNumber{0};
    std::uint64_t metadataVersion{0};
    CentralState state{CentralState::Submitted};
    // canonicalConfigSha256 of the embedded config.json: the coordinator
    // matches the applied config.json against it (#398 M2).
    std::string configSha256;
    // Directory holding the verified materialized files; empty when not
    // materialized (or the files no longer match the revision).
    std::string materializedDir;
};

// An operator's explicit local validation of a cached revision (#398 M2):
// "this revision ran correctly on this instrument/context; here is the test
// run". The validator is the signed-in registry user.
struct LocalValidationRequest {
    std::string revisionId;
    MethodContext context;      // context.instrumentId required
    std::string instrumentName; // label recorded in the evidence (may be empty)
    std::string evidenceFile;   // test-run file recorded with this revision applied
    bool passed{true};          // false records a failed validation (blocks nothing; gate warns)
};

struct RegistryWorkerSnapshot {
    enum class Session {
        SignedOut,
        SignedIn,
        // The last user's cache is open from last_session.json; no tokens.
        // Cached revisions are listable; network commands need sign-in.
        CachedOffline,
    };
    std::uint64_t generation{0}; // bumps on every published change
    bool configured{false};
    std::string origin;
    Session session{Session::SignedOut};
    std::string subjectId;
    std::string email;
    // Registry connectivity only. Never an instrument fault.
    RegistryHealth health;
    std::vector<RegistryProject> projects;
    std::vector<CachedRevisionSummary> revisions;
    std::vector<LocalValidationRecord> validations; // newest first, every instrument
    std::vector<std::string> corruptRevisionIds; // failed verification on read
    std::string cacheError;                      // cache could not be opened/read
    std::optional<std::chrono::system_clock::time_point> lastSuccessfulRefresh;
    RegistryJobStatus lastJob;
    std::size_t queuedJobs{0};
    bool busy{false};
};
const char* toString(RegistryWorkerSnapshot::Session session);

class ProfileRegistryWorker {
public:
    // An unconfigured worker starts no thread and refuses every request; its
    // snapshot says configured=false.
    ProfileRegistryWorker(RegistryWorkerConfig config, RegistryHttpTransport transport);
    ~ProfileRegistryWorker(); // shutdown()
    ProfileRegistryWorker(const ProfileRegistryWorker&) = delete;
    ProfileRegistryWorker& operator=(const ProfileRegistryWorker&) = delete;

    // Each returns the job ID, or 0 when refused (unconfigured / shut down /
    // invalid argument). An identical queued refresh is coalesced.
    std::uint64_t requestSignIn(std::string email, std::string password);
    std::uint64_t requestSignOut();
    std::uint64_t requestRefresh(); // every project the user is a member of
    std::uint64_t requestDownload(std::string revisionId);
    // Writes a cached, verified revision's files under methodsDir (idempotent).
    std::uint64_t requestMaterialize(std::string revisionId);
    // Hashes the evidence file (cancellable) and records the validation.
    // Needs a signed-in session; refused (0) without instrument id or file.
    std::uint64_t requestRecordValidation(LocalValidationRequest request);

    // Drops queued jobs (Cancelled) and aborts the running one. Idempotent.
    void cancelAll();

    RegistryWorkerSnapshot snapshot() const;
    // snapshot().generation without copying (cheap change detection).
    std::uint64_t generation() const;
    RegistryJobStatus job(std::uint64_t id) const;
    bool waitForJob(std::uint64_t id, std::chrono::milliseconds timeout) const; // tests/tools
    bool waitIdle(std::chrono::milliseconds timeout) const;                     // tests/tools

    void shutdown(); // idempotent; joins the worker thread

private:
    struct Command {
        std::uint64_t id{0};
        RegistryJobKind kind{RegistryJobKind::Refresh};
        std::string argument; // email (sign-in) or revision ID (download)
        std::string secret;   // password (sign-in only); cleared once taken
        std::optional<LocalValidationRequest> validation;
    };
    struct Active; // per-user cache + provider + service (worker thread only)

    std::uint64_t enqueue(Command command);
    void run();
    RegistryJobStatus execute(Command& command);
    RegistryJobStatus doSignIn(const std::string& email, std::string& password);
    RegistryJobStatus doSignOut();
    RegistryJobStatus doRefresh();
    RegistryJobStatus doDownload(const std::string& revisionId);
    RegistryJobStatus doMaterialize(const std::string& revisionId);
    RegistryJobStatus doRecordValidation(const LocalValidationRequest& request);
    void scanMaterialized(); // methodsDir entries matching this user's cache
    bool openUser(const std::string& subject, const std::string& email, bool persistLastSession);
    void closeUser();
    void loadLastSession();
    // Runs `op` (a service call returning false on failure); on an
    // authentication failure, refreshes the session once and retries.
    bool withSession(const std::function<bool()>& op);
    bool refreshSession();
    void noteSuccess();
    void noteFailure(RegistryErrorCode code, const std::string& message);
    void noteService(bool ok); // copy the active service's outcome into health_
    bool cancelRequested() const;
    void publishLocked(); // worker state -> published_ (mutex_ held)
    void publish();
    void setJobLocked(const RegistryJobStatus& status);
    std::filesystem::path cachePathFor(const std::string& subject) const;

    const RegistryWorkerConfig config_;
    const RegistryHttpTransport transport_;

    // Worker thread only.
    std::unique_ptr<SupabaseAuth> auth_;
    AuthSession session_;
    std::unique_ptr<Active> active_;
    std::string subjectId_;
    std::string email_;
    RegistryHealth health_;
    std::vector<RegistryProject> projects_;
    std::string cacheError_;
    std::optional<std::chrono::system_clock::time_point> lastSuccessfulRefresh_;
    std::map<std::string, std::string> materialized_; // revisionId -> dir (verified)

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::deque<Command> queue_;
    std::map<std::uint64_t, RegistryJobStatus> jobs_; // bounded: recent statuses
    std::uint64_t nextJobId_{1};
    std::uint64_t runningJob_{0};
    bool stopping_{false};
    bool loading_{false}; // initial last-session load still running
    bool usable_{false};  // configured with a valid endpoint
    RegistryWorkerSnapshot published_;
    std::atomic<bool> cancelRunning_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;
};

} // namespace backend::profiles
