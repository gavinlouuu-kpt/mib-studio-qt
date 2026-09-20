#include "backend/supervisor/SupervisorService.h"

#include "backend/app/Tools.h"
#include "backend/supervisor/SafetyPolicy.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>

namespace backend::supervisor {

namespace {

uint64_t wallClockNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

DecisionRecord decideOnce(const ExperimentSnapshot& snapshot, const DecisionPolicy& policy,
                          DecisionProvider* provider, SupervisorMode mode)
{
    DecisionRecord rec;
    rec.sequence = snapshot.sequence;
    rec.wallClockNs = wallClockNs();
    rec.hostTimeUs = Tools::getTimestamp();
    rec.mode = toString(mode);
    rec.runId = snapshot.runId;
    rec.snapshot = snapshot;
    rec.snapshotSchemaVersion = snapshot.schemaVersion;
    rec.snapshotHash = snapshotHash(snapshot);
    rec.executed = false; // shadow mode: structurally no actuation

    rec.policy = evaluateSafetyPolicy(snapshot, policy);
    if (rec.policy.requiresAction) {
        rec.recommendation = rec.policy.answers;
        rec.decidedBy = "policy";
        rec.providerConsulted = false;
        rec.provider.answers = humanReviewAnswers();
        rec.eligibility.eligible = false;
        rec.eligibility.reason = "deterministic policy decided (" + rec.policy.ruleId + ")";
        return rec;
    }

    if (!provider) {
        rec.recommendation = humanReviewAnswers(PrimaryProblem::Other);
        rec.decidedBy = "fail_closed";
        rec.provider.error = {ProviderErrorKind::NotConfigured, "no decision provider installed"};
        rec.eligibility.reason = "no provider";
        return rec;
    }

    rec.providerConsulted = true;
    try {
        rec.provider = provider->evaluate(snapshot, policy);
    } catch (const std::exception& e) {
        rec.provider = DecisionResult{};
        rec.provider.providerName = provider->name();
        rec.provider.providerVersion = provider->version();
        rec.provider.modelVersion = provider->modelVersion();
        rec.provider.error = {ProviderErrorKind::Exception, std::string("provider threw: ") + e.what()};
    } catch (...) {
        rec.provider = DecisionResult{};
        rec.provider.providerName = provider->name();
        rec.provider.error = {ProviderErrorKind::Exception, "provider threw a non-std exception"};
    }
    if (rec.provider.providerName.empty()) rec.provider.providerName = provider->name();
    if (rec.provider.providerVersion.empty()) rec.provider.providerVersion = provider->version();
    if (rec.provider.modelVersion.empty()) rec.provider.modelVersion = provider->modelVersion();

    if (!rec.provider.ok()) {
        // Fail closed: provider timeout/error -> no autonomous action.
        rec.provider.answers = humanReviewAnswers(PrimaryProblem::Other);
        rec.recommendation = rec.provider.answers;
        rec.decidedBy = "fail_closed";
        rec.eligibility = evaluateEligibility(rec.provider, policy);
        return rec;
    }

    rec.recommendation = rec.provider.answers;
    rec.decidedBy = "provider";
    rec.eligibility = evaluateEligibility(rec.provider, policy);
    return rec;
}

// ---- SupervisorService ---------------------------------------------------------

SupervisorService::SupervisorService() = default;

SupervisorService::~SupervisorService() { shutdown(); }

bool SupervisorService::setProvider(std::unique_ptr<DecisionProvider> provider)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_) return false;
    provider_ = std::move(provider);
    if (provider_) {
        stats_.providerName = provider_->name();
        stats_.providerVersion = provider_->version();
        stats_.modelVersion = provider_->modelVersion();
    } else {
        stats_.providerName.clear();
        stats_.providerVersion.clear();
        stats_.modelVersion.clear();
    }
    return true;
}

DecisionProvider* SupervisorService::provider() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return provider_.get();
}

void SupervisorService::setSnapshotSource(SnapshotSource source)
{
    std::lock_guard<std::mutex> lk(mutex_);
    source_ = std::move(source);
}

uint64_t SupervisorService::addObserver(RecordObserver observer)
{
    std::lock_guard<std::mutex> lk(observersMutex_);
    const auto id = nextObserverId_++;
    observers_.emplace_back(id, std::move(observer));
    return id;
}

void SupervisorService::removeObserver(uint64_t id)
{
    std::lock_guard<std::mutex> lk(observersMutex_);
    for (auto it = observers_.begin(); it != observers_.end(); ++it) {
        if (it->first == id) { observers_.erase(it); return; }
    }
}

bool SupervisorService::configure(const SupervisorConfig& config, std::string* error)
{
    if (config.intervalMs < config.minIntervalMs) {
        if (error) *error = "intervalMs below minIntervalMs";
        return false;
    }
    if (config.policy.providerTimeoutMs <= 0 || config.policy.providerTimeoutMs > 120000) {
        if (error) *error = "providerTimeoutMs must be in (0, 120000]";
        return false;
    }
    if (config.policy.providerMaxRetries < 0 || config.policy.providerMaxRetries > 3) {
        if (error) *error = "providerMaxRetries must be in [0, 3]";
        return false;
    }
    if (config.policy.hardFrameLossFraction <= 0.0 || config.policy.hardFrameLossFraction > 1.0) {
        if (error) *error = "hardFrameLossFraction must be in (0, 1]";
        return false;
    }
    bool stopWorker = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        config_ = config;
        stats_.mode = config.mode;
        stopWorker = running_ && config.mode == SupervisorMode::Off;
    }
    if (stopWorker) requestStop();
    cv_.notify_all();
    return true;
}

SupervisorConfig SupervisorService::config() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return config_;
}

bool SupervisorService::start(std::string* error)
{
    std::unique_lock<std::mutex> lk(mutex_);
    if (running_) return true;
    if (config_.mode != SupervisorMode::Shadow) {
        if (error) *error = "supervisor mode is off";
        return false;
    }
    if (!provider_) {
        if (error) *error = "no decision provider installed";
        return false;
    }
    if (!source_) {
        if (error) *error = "no snapshot source installed";
        return false;
    }
    if (worker_.joinable()) {
        // A previous run's worker has exited; reap it first.
        workerExit_ = true;
        cv_.notify_all();
        lk.unlock();
        worker_.join();
        lk.lock();
    }
    if (!config_.logPath.empty()) {
        std::string err;
        if (!log_.open(config_.logPath, config_.runId, config_.mode, provider_->name(),
                       provider_->version(), provider_->modelVersion(), &err)) {
            stats_.lastError = err;
            ++stats_.logWriteFailures;
            SPDLOG_WARN("Supervisor: {} (continuing without sidecar)", err);
        }
    }
    workerExit_ = false;
    running_ = true;
    stats_.running = true;
    priors_.clear();
    provider_->resetCancel();
    worker_ = std::thread([this] { worker(); });
    SPDLOG_INFO("Supervisor: shadow mode started (provider={}, interval={} ms) — recommendations only, no actuation",
                provider_->name(), config_.intervalMs);
    return true;
}

void SupervisorService::requestStop()
{
    DecisionProvider* p = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        workerExit_ = true;
        p = provider_.get();
    }
    // Unconditional: a call that becomes in-flight right after this check
    // would otherwise run to its full timeout before shutdown() can join.
    if (p) p->cancel();
    cv_.notify_all();
}

void SupervisorService::shutdown()
{
    requestStop();
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        t = std::move(worker_);
    }
    if (t.joinable()) t.join();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        running_ = false;
        stats_.running = false;
    }
    log_.close();
}

bool SupervisorService::isRunning() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return running_;
}

void SupervisorService::worker()
{
    uint64_t sequence = 0;
    for (;;) {
        int intervalMs = 0;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (workerExit_) break;
            intervalMs = config_.intervalMs;
            sequence = nextSequence_++;
        }
        tick(sequence);
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, std::chrono::milliseconds(intervalMs), [this] { return workerExit_; });
        if (workerExit_) break;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    running_ = false;
    stats_.running = false;
}

void SupervisorService::tick(uint64_t sequence)
{
    SnapshotSource source;
    DecisionPolicy policy;
    bool onlyActive = true;
    SupervisorMode mode = SupervisorMode::Shadow;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        source = source_;
        policy = config_.policy;
        onlyActive = config_.onlyWhileExperimentActive;
        mode = config_.mode;
    }
    if (!source) return;
    std::optional<ExperimentSnapshot> snapshot;
    try {
        snapshot = source(sequence);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(mutex_);
        stats_.lastError = std::string("snapshot source threw: ") + e.what();
        return;
    }
    if (!snapshot) return;
    if (onlyActive && snapshot->experimentState != "active") {
        std::lock_guard<std::mutex> lk(mutex_);
        ++stats_.skippedInactive;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        snapshot->priorRecommendations = priors_;
    }
    DecisionProvider* provider = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (workerExit_) return; // stop requested while the snapshot was built
        provider = provider_.get();
        inFlight_.store(true, std::memory_order_release);
    }
    DecisionRecord rec = decideOnce(*snapshot, policy, provider, mode);
    inFlight_.store(false, std::memory_order_release);
    rec = finishRecord(std::move(rec));
    notify(rec);
}

DecisionRecord SupervisorService::evaluateNow(const ExperimentSnapshot& snapshot)
{
    DecisionPolicy policy;
    SupervisorMode mode = SupervisorMode::Shadow;
    DecisionProvider* provider = nullptr;
    ExperimentSnapshot copy = snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        policy = config_.policy;
        mode = config_.mode == SupervisorMode::Off ? SupervisorMode::Shadow : config_.mode;
        provider = provider_.get();
        if (copy.sequence == 0) copy.sequence = nextSequence_++;
        copy.priorRecommendations = priors_;
    }
    inFlight_.store(true, std::memory_order_release);
    DecisionRecord rec = decideOnce(copy, policy, provider, mode);
    inFlight_.store(false, std::memory_order_release);
    rec = finishRecord(std::move(rec));
    notify(rec);
    return rec;
}

DecisionRecord SupervisorService::finishRecord(DecisionRecord rec)
{
    std::string err;
    const bool logged = !log_.isOpen() || log_.append(rec, &err);
    std::lock_guard<std::mutex> lk(mutex_);
    ++stats_.evaluations;
    if (rec.decidedBy == "policy") ++stats_.policyDecisions;
    else if (rec.decidedBy == "provider") ++stats_.providerDecisions;
    if (rec.providerConsulted && !rec.provider.ok()) {
        ++stats_.providerFailures;
        stats_.lastError = std::string(toString(rec.provider.error.kind)) + ": " + rec.provider.error.message;
    }
    if (!logged) {
        ++stats_.logWriteFailures;
        stats_.lastError = err;
    }
    stats_.lastLatencyUs = rec.provider.latencyUs;
    stats_.lastRecord = rec;
    recent_.push_back(rec);
    while (recent_.size() > config_.retainedRecords && !recent_.empty()) recent_.erase(recent_.begin());
    PriorRecommendation prior;
    prior.sequence = rec.sequence;
    prior.elapsedSeconds = static_cast<uint64_t>(rec.snapshot.elapsedSeconds);
    prior.action = toString(rec.recommendation.action);
    prior.target = toString(rec.recommendation.target);
    prior.direction = toString(rec.recommendation.direction);
    prior.executed = false;
    priors_.push_back(prior);
    while (priors_.size() > 8) priors_.erase(priors_.begin());
    return rec;
}

void SupervisorService::notify(const DecisionRecord& record)
{
    std::vector<RecordObserver> copy;
    {
        std::lock_guard<std::mutex> lk(observersMutex_);
        for (const auto& [id, fn] : observers_) copy.push_back(fn);
    }
    for (const auto& fn : copy) {
        try { fn(record); } catch (...) { SPDLOG_WARN("Supervisor: observer threw"); }
    }
}

void SupervisorService::recordOperatorAction(const std::string& action)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (recent_.empty()) return;
    auto& last = recent_.back();
    last.operatorAction = action;
    last.operatorActionWallClockNs = wallClockNs();
    if (stats_.lastRecord && stats_.lastRecord->sequence == last.sequence) stats_.lastRecord = last;
    // Persist the association as a follow-up line; the original record is
    // immutable once written.
    if (log_.isOpen()) {
        DecisionRecord assoc = last;
        std::string err;
        if (!log_.append(assoc, &err)) { ++stats_.logWriteFailures; stats_.lastError = err; }
    }
}

SupervisorStatus SupervisorService::status() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    SupervisorStatus s = stats_;
    s.mode = config_.mode;
    s.running = running_;
    s.inFlight = inFlight_.load(std::memory_order_acquire);
    s.recommendationOnly = true;
    return s;
}

std::vector<DecisionRecord> SupervisorService::recentRecords() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return recent_;
}

} // namespace backend::supervisor
