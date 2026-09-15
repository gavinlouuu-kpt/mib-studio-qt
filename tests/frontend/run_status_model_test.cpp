// run_status_model_test (issue #363)
//
// Pure model semantics: repeated same-key faults aggregate into one entry;
// acknowledging is not resolving; the history cap never drops an unresolved
// error; a stale operation cannot change a newer run's phase; a latched
// failure turns a later Complete into Failed; every phase has a distinct
// text label (no color-only semantics).

#include "frontend/models/RunStatusModel.h"

#include "support/assert.h"

#include <QCoreApplication>
#include <QSet>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    using namespace frontend;

    // ---- alerts ---------------------------------------------------------------
    UiAlertModel alerts;
    int changes = 0;
    QObject::connect(&alerts, &UiAlertModel::changed, [&] { ++changes; });
    for (int i = 0; i < 100; ++i) alerts.raise(QStringLiteral("camera.start"), AlertSeverity::Error, QStringLiteral("camera start failed #%1").arg(i), QStringLiteral("check the cable"));
    MIB_EXPECT(alerts.all().size() == 1 && alerts.find(QStringLiteral("camera.start"))->count == 100, "100 repeats -> one entry with count 100");
    MIB_EXPECT(alerts.find(QStringLiteral("camera.start"))->message.endsWith(QStringLiteral("#99")), "message refreshed to the latest");
    MIB_EXPECT(changes == 100, "one change per raise (UI coalesces)");
    alerts.acknowledge(QStringLiteral("camera.start"));
    MIB_EXPECT(alerts.unacknowledged().isEmpty() && alerts.unresolvedCount() == 1 && alerts.headline() == nullptr,
               "acknowledged alert leaves the banner but stays unresolved");
    alerts.raise(QStringLiteral("camera.start"), AlertSeverity::Error, QStringLiteral("again"));
    MIB_EXPECT(alerts.headline() && alerts.find(QStringLiteral("camera.start"))->count == 101, "a repeat re-surfaces an acknowledged alert");
    alerts.resolve(QStringLiteral("camera.start"));
    MIB_EXPECT(alerts.unresolvedCount() == 0 && alerts.headline() == nullptr, "resolve clears it");
    // Headline picks the highest severity, then the most recent.
    alerts.raise(QStringLiteral("a.warn"), AlertSeverity::Warning, QStringLiteral("w"));
    alerts.raise(QStringLiteral("b.crit"), AlertSeverity::Critical, QStringLiteral("c"));
    alerts.raise(QStringLiteral("c.warn"), AlertSeverity::Warning, QStringLiteral("w2"));
    MIB_EXPECT(alerts.headline()->key == QStringLiteral("b.crit"), "headline = highest severity");
    // Cap: 60 distinct informational alerts + one unresolved error -> error retained.
    for (int i = 0; i < 60; ++i) alerts.raise(QStringLiteral("info.%1").arg(i), AlertSeverity::Info, QStringLiteral("i"));
    MIB_EXPECT(alerts.all().size() <= UiAlertModel::kMaxRetained, "history capped");
    MIB_EXPECT(alerts.find(QStringLiteral("b.crit")) != nullptr && alerts.overflowDropped() > 0, "blocking alert survives the cap; overflow counted");
    alerts.acknowledgeAll();
    MIB_EXPECT(alerts.headline() == nullptr && alerts.unresolvedCount(AlertSeverity::Error) == 1, "acknowledge-all hides but keeps the critical unresolved");
    alerts.clearResolved();
    MIB_EXPECT(alerts.find(QStringLiteral("b.crit")) != nullptr, "unresolved alerts are never cleared by clearResolved");

    // ---- run status ---------------------------------------------------------
    RunStatusModel run;
    const uint64_t op1 = run.beginOperation(RunPhase::Starting, QStringLiteral("run1.h5"));
    MIB_EXPECT(run.setPhase(RunPhase::Running, op1), "phase for current op");
    MIB_EXPECT(!run.setPhase(RunPhase::Complete, op1 + 5), "unknown/future op rejected");
    const uint64_t op2 = run.beginOperation(RunPhase::Starting);
    MIB_EXPECT(!run.setPhase(RunPhase::Complete, op1), "stale op1 completion rejected after op2 began");
    MIB_EXPECT(run.state().phase == RunPhase::Starting && run.state().operationId == op2, "op2 untouched");
    run.setPhase(RunPhase::Running, op2);
    MIB_EXPECT(run.latchFailure(op2, QStringLiteral("flush failed")), "failure latched");
    // The real stop order: the fatal error latches first, then the stop
    // walks Stopping -> Saving (no detail) -> Complete.
    run.setPhase(RunPhase::Stopping, op2);
    MIB_EXPECT(run.state().phase == RunPhase::Stopping && run.state().failureLatched, "intermediate phases still shown while latched");
    run.setPhase(RunPhase::Saving, op2);
    run.latchFailure(op2, QStringLiteral("metadata failed"));
    MIB_EXPECT(run.setPhase(RunPhase::Complete, op2), "phase accepted");
    MIB_EXPECT(run.state().phase == RunPhase::Failed && run.state().failureLatched, "later Complete cannot un-fail a latched run");
    MIB_EXPECT(run.state().text().contains(QStringLiteral("flush failed")), "the first cause is named by the final Failed state");
    run.setIdlePhase(RunPhase::CameraRunning);
    MIB_EXPECT(run.state().phase == RunPhase::Failed, "idle-class phases do not hide a latched failure");
    run.beginOperation(RunPhase::Starting);
    MIB_EXPECT(!run.state().failureLatched && run.state().phase == RunPhase::Starting, "new operation clears the latch");
    run.setIdlePhase(RunPhase::Idle);
    MIB_EXPECT(run.state().text() == runPhaseLabel(RunPhase::Idle), "idle text");
    QSet<QString> labels, glyphs;
    for (RunPhase p : {RunPhase::Idle, RunPhase::CameraRunning, RunPhase::Starting, RunPhase::Running, RunPhase::Stopping,
                       RunPhase::Saving, RunPhase::Complete, RunPhase::Failed}) {
        labels.insert(runPhaseLabel(p));
        glyphs.insert(runPhaseGlyph(p));
        MIB_EXPECT(!runPhaseLabel(p).isEmpty(), "every phase has text");
    }
    MIB_EXPECT(labels.size() == 8 && glyphs.size() == 8, "phase labels and glyphs are distinct");

    return mib::test::exitCode();
}
