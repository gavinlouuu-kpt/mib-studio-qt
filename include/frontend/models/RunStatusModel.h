// Run presentation state + alert model (issue #363).
//
// Two small, testable QObjects that own what the status bar used to mix into
// one string:
//  - RunStatusModel: the authoritative *projection* of the run lifecycle
//    (Idle / CameraRunning / Starting / Running / Stopping / Saving /
//    Complete / Failed) tagged with an operation id, so a stale completion
//    can never overwrite a newer run, and a latched failure survives later
//    partial successes.
//  - UiAlertModel: actionable warnings/errors keyed by source/code. Repeats
//    aggregate into one entry (count, first/last time); acknowledging hides
//    an alert from the banner but does not resolve it; only the owner of the
//    underlying condition resolves it. History is capped with an explicit
//    overflow counter; blocking (error/critical, unresolved) alerts are never
//    dropped by the cap.
// Metrics updates never add or remove alerts.
#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>

namespace frontend {

enum class RunPhase { Idle, CameraRunning, Starting, Running, Stopping, Saving, Complete, Failed };

const char* toString(RunPhase phase);
// Text + glyph presentation (never color alone).
QString runPhaseGlyph(RunPhase phase);
QString runPhaseLabel(RunPhase phase);

struct RunPresentationState {
    RunPhase phase{RunPhase::Idle};
    uint64_t operationId{0};     // run/session identity the phase belongs to
    QString detail;              // short human text (failure reason, file)
    bool failureLatched{false};  // a failure was recorded for operationId
    QString text() const;        // "Running", "Failed: <detail>"
};

class RunStatusModel : public QObject {
    Q_OBJECT
public:
    explicit RunStatusModel(QObject* parent = nullptr);

    const RunPresentationState& state() const { return state_; }
    uint64_t currentOperation() const { return state_.operationId; }

    // Start a new operation (run). Returns its id.
    uint64_t beginOperation(RunPhase phase, const QString& detail = {});
    // Phase change for an operation; a stale id (older than current) is
    // rejected and returns false. Once a failure is latched for the
    // operation, Complete is reported as Failed.
    bool setPhase(RunPhase phase, uint64_t operationId, const QString& detail = {});
    // Convenience for lifecycle phases that belong to no run (Idle/CameraRunning).
    void setIdlePhase(RunPhase phase, const QString& detail = {});
    // Latch a failure for the operation (stays until a new operation begins).
    bool latchFailure(uint64_t operationId, const QString& reason);

signals:
    void changed(const frontend::RunPresentationState& state);

private:
    RunPresentationState state_;
    QString failureReason_; // first latched reason for the current operation
    uint64_t nextOperation_{1};
};

enum class AlertSeverity { Info, Warning, Error, Critical };
const char* toString(AlertSeverity s);

struct UiAlert {
    QString key;            // "source.code", e.g. "save.fatal"
    AlertSeverity severity{AlertSeverity::Warning};
    QString message;
    QString remediation;
    qint64 firstAtMs{0};
    qint64 lastAtMs{0};
    int count{1};
    bool acknowledged{false};
    bool resolved{false};
    bool blocking() const { return !resolved && severity >= AlertSeverity::Error; }
};

class UiAlertModel : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxRetained = 50;

    explicit UiAlertModel(QObject* parent = nullptr);

    // Raise (or repeat) an alert. Repeats of the same key aggregate: count++,
    // lastAt updated, message refreshed, resolved/acknowledged cleared.
    void raise(const QString& key, AlertSeverity severity, const QString& message, const QString& remediation = {});
    // Operator acknowledged: hidden from the banner, still unresolved.
    void acknowledge(const QString& key);
    void acknowledgeAll();
    // The owning condition cleared.
    void resolve(const QString& key);
    void clearResolved();

    QList<UiAlert> all() const { return alerts_; }
    QList<UiAlert> unresolved() const;
    QList<UiAlert> unacknowledged() const; // unresolved && !acknowledged
    const UiAlert* find(const QString& key) const;
    int unresolvedCount(AlertSeverity atLeast = AlertSeverity::Info) const;
    int overflowDropped() const { return overflowDropped_; }
    // Highest-severity unacknowledged alert (nullptr when none).
    const UiAlert* headline() const;

signals:
    void changed();

private:
    void enforceCap();
    QList<UiAlert> alerts_;
    int overflowDropped_{0};
};

} // namespace frontend
