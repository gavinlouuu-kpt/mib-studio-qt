#include "frontend/models/RunStatusModel.h"

#include <algorithm>

namespace frontend {

const char* toString(RunPhase phase)
{
    switch (phase) {
    case RunPhase::Idle: return "idle";
    case RunPhase::CameraRunning: return "cameraRunning";
    case RunPhase::Starting: return "starting";
    case RunPhase::Running: return "running";
    case RunPhase::Stopping: return "stopping";
    case RunPhase::Saving: return "saving";
    case RunPhase::Complete: return "complete";
    case RunPhase::Failed: return "failed";
    }
    return "unknown";
}

QString runPhaseGlyph(RunPhase phase)
{
    switch (phase) {
    case RunPhase::Idle: return QStringLiteral("○");
    case RunPhase::CameraRunning: return QStringLiteral("◉");
    case RunPhase::Starting: return QStringLiteral("◔");
    case RunPhase::Running: return QStringLiteral("●");
    case RunPhase::Stopping: return QStringLiteral("◑");
    case RunPhase::Saving: return QStringLiteral("⇩");
    case RunPhase::Complete: return QStringLiteral("✔");
    case RunPhase::Failed: return QStringLiteral("✕");
    }
    return QStringLiteral("?");
}

QString runPhaseLabel(RunPhase phase)
{
    switch (phase) {
    case RunPhase::Idle: return QObject::tr("Idle");
    case RunPhase::CameraRunning: return QObject::tr("Camera running");
    case RunPhase::Starting: return QObject::tr("Starting");
    case RunPhase::Running: return QObject::tr("Running");
    case RunPhase::Stopping: return QObject::tr("Stopping");
    case RunPhase::Saving: return QObject::tr("Saving");
    case RunPhase::Complete: return QObject::tr("Complete");
    case RunPhase::Failed: return QObject::tr("Failed – recovery required");
    }
    return QObject::tr("Unknown");
}

QString RunPresentationState::text() const
{
    const QString label = runPhaseLabel(phase);
    return detail.isEmpty() ? label : QStringLiteral("%1: %2").arg(label, detail);
}

RunStatusModel::RunStatusModel(QObject* parent) : QObject(parent) {}

uint64_t RunStatusModel::beginOperation(RunPhase phase, const QString& detail)
{
    state_ = RunPresentationState{};
    failureReason_.clear();
    state_.operationId = nextOperation_++;
    state_.phase = phase;
    state_.detail = detail;
    emit changed(state_);
    return state_.operationId;
}

bool RunStatusModel::setPhase(RunPhase phase, uint64_t operationId, const QString& detail)
{
    if (operationId != state_.operationId) return false; // stale (or future) operation
    if (state_.failureLatched && phase == RunPhase::Complete) phase = RunPhase::Failed;
    state_.phase = phase;
    // Intermediate phases (Stopping/Saving) may carry no detail; the latched
    // reason is kept aside so the final Failed state always names its cause.
    state_.detail = (phase == RunPhase::Failed && detail.isEmpty()) ? failureReason_ : detail;
    emit changed(state_);
    return true;
}

void RunStatusModel::setIdlePhase(RunPhase phase, const QString& detail)
{
    // Idle-class phases do not belong to a run; a latched failure of the last
    // run stays visible until the operator starts something new.
    if (state_.failureLatched && state_.phase == RunPhase::Failed) return;
    state_.phase = phase;
    state_.detail = detail;
    emit changed(state_);
}

bool RunStatusModel::latchFailure(uint64_t operationId, const QString& reason)
{
    if (operationId != state_.operationId) return false;
    state_.failureLatched = true;
    if (failureReason_.isEmpty()) failureReason_ = reason; // first cause wins
    state_.phase = RunPhase::Failed;
    state_.detail = failureReason_;
    emit changed(state_);
    return true;
}

// ---------------------------------------------------------------------------

const char* toString(AlertSeverity s)
{
    switch (s) {
    case AlertSeverity::Info: return "info";
    case AlertSeverity::Warning: return "warning";
    case AlertSeverity::Error: return "error";
    case AlertSeverity::Critical: return "critical";
    }
    return "unknown";
}

UiAlertModel::UiAlertModel(QObject* parent) : QObject(parent) {}

void UiAlertModel::raise(const QString& key, AlertSeverity severity, const QString& message, const QString& remediation)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (UiAlert& a : alerts_) {
        if (a.key == key) {
            ++a.count;
            a.lastAtMs = now;
            a.message = message;
            if (!remediation.isEmpty()) a.remediation = remediation;
            a.severity = std::max(a.severity, severity);
            a.resolved = false;
            a.acknowledged = false;
            emit changed();
            return;
        }
    }
    UiAlert a;
    a.key = key;
    a.severity = severity;
    a.message = message;
    a.remediation = remediation;
    a.firstAtMs = now;
    a.lastAtMs = now;
    alerts_.push_back(a);
    enforceCap();
    emit changed();
}

void UiAlertModel::acknowledge(const QString& key)
{
    for (UiAlert& a : alerts_) {
        if (a.key == key && !a.acknowledged) {
            a.acknowledged = true;
            emit changed();
            return;
        }
    }
}

void UiAlertModel::acknowledgeAll()
{
    bool any = false;
    for (UiAlert& a : alerts_) {
        if (!a.resolved && !a.acknowledged) { a.acknowledged = true; any = true; }
    }
    if (any) emit changed();
}

void UiAlertModel::resolve(const QString& key)
{
    for (UiAlert& a : alerts_) {
        if (a.key == key && !a.resolved) {
            a.resolved = true;
            emit changed();
            return;
        }
    }
}

void UiAlertModel::clearResolved()
{
    const int before = alerts_.size();
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(), [](const UiAlert& a) { return a.resolved; }), alerts_.end());
    if (alerts_.size() != before) emit changed();
}

QList<UiAlert> UiAlertModel::unresolved() const
{
    QList<UiAlert> out;
    for (const UiAlert& a : alerts_) if (!a.resolved) out.push_back(a);
    return out;
}

QList<UiAlert> UiAlertModel::unacknowledged() const
{
    QList<UiAlert> out;
    for (const UiAlert& a : alerts_) if (!a.resolved && !a.acknowledged) out.push_back(a);
    return out;
}

const UiAlert* UiAlertModel::find(const QString& key) const
{
    for (const UiAlert& a : alerts_) if (a.key == key) return &a;
    return nullptr;
}

int UiAlertModel::unresolvedCount(AlertSeverity atLeast) const
{
    int n = 0;
    for (const UiAlert& a : alerts_) if (!a.resolved && a.severity >= atLeast) ++n;
    return n;
}

const UiAlert* UiAlertModel::headline() const
{
    const UiAlert* best = nullptr;
    for (const UiAlert& a : alerts_) {
        if (a.resolved || a.acknowledged) continue;
        if (!best || a.severity > best->severity || (a.severity == best->severity && a.lastAtMs > best->lastAtMs)) best = &a;
    }
    return best;
}

void UiAlertModel::enforceCap()
{
    // Drop the oldest resolved/acknowledged informational entries first;
    // never drop an unresolved error/critical alert.
    while (alerts_.size() > kMaxRetained) {
        auto victim = std::find_if(alerts_.begin(), alerts_.end(), [](const UiAlert& a) { return a.resolved; });
        if (victim == alerts_.end())
            victim = std::find_if(alerts_.begin(), alerts_.end(), [](const UiAlert& a) { return !a.blocking(); });
        if (victim == alerts_.end()) break; // everything left is blocking: keep (explicit policy)
        alerts_.erase(victim);
        ++overflowDropped_;
    }
}

} // namespace frontend
