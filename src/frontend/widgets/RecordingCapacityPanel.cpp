#include "frontend/widgets/RecordingCapacityPanel.h"

#include "backend/processing/ExperimentBufferSizing.h"

#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace frontend {
namespace {

using backend::app::GateStatus;
using backend::app::ReadinessGate;

int severity(GateStatus s)
{
    switch (s) {
    case GateStatus::Fail:
    case GateStatus::Unavailable: return 2;
    case GateStatus::Warn: return 1;
    default: return 0;
    }
}

QString statusWord(int level)
{
    return level >= 2 ? QStringLiteral("Blocked") : level == 1 ? QStringLiteral("Warning") : QStringLiteral("OK");
}

QString q(const std::string& s) { return QString::fromStdString(s); }

QString gateLine(const ReadinessGate& g)
{
    QString line = statusWord(severity(g.status)) + QStringLiteral(" - ") + q(g.id) + QStringLiteral(": ");
    if (g.status == GateStatus::Pass || g.reason.empty()) {
        line += q(g.detail);
    } else {
        line += q(g.reason);
        if (!g.remediation.empty()) line += QStringLiteral(" -> ") + q(g.remediation);
    }
    return line;
}

} // namespace

RecordingCapacityText describeRecordingCapacity(const backend::app::ExperimentCoordinator::RecordingCapacityView& view)
{
    RecordingCapacityText t;
    std::vector<const ReadinessGate*> gates;
    if (!view.window.id.empty()) gates.push_back(&view.window);
    if (view.known) {
        gates.push_back(&view.capacity.rate);
        gates.push_back(&view.capacity.buffer);
        gates.push_back(&view.capacity.throughput);
    }
    int worst = 0;
    for (const auto* g : gates) worst = std::max(worst, severity(g->status));
    std::stable_sort(gates.begin(), gates.end(),
                     [](const ReadinessGate* a, const ReadinessGate* b) { return severity(a->status) > severity(b->status); });
    for (const auto* g : gates) t.gates << gateLine(*g);

    if (!view.known) {
        t.status = QStringLiteral("Unknown");
        t.summary = QStringLiteral("Buffer: start Live View to size it");
        t.shortSummary = QStringLiteral("Buffer: --");
        t.facts << QStringLiteral("The camera window is not known yet: start Live View.");
        return t;
    }
    t.status = statusWord(worst);
    const auto& in = view.inputs;
    const auto& plan = in.plan;
    t.facts << QStringLiteral("%1 %2x%3, %4 images per saved frame (%5)")
                   .arg(view.planned ? QStringLiteral("Planned window") : QStringLiteral("Window"))
                   .arg(in.windowWidth)
                   .arg(in.windowHeight)
                   .arg(in.imagesPerFrame)
                   .arg(q(backend::processing::formatBufferBytes(plan.bytesPerFrame)));
    if (in.requestedFps > 0) {
        QString rate = QStringLiteral("Rate %1 fps requested (%2)").arg(in.requestedFps, 0, 'f', 0).arg(q(in.requestedFpsSource));
        rate += in.maxFps > 0 ? QStringLiteral(", camera max %1 fps").arg(in.maxFps, 0, 'f', 0)
                              : view.planned ? QStringLiteral(", camera max read when the Experiment script applies the window")
                                             : QStringLiteral(", camera max unknown");
        t.facts << rate;
        t.facts << QStringLiteral("Save stream: every frame %1, no cells %2 (every %3th invalid frame)")
                       .arg(q(backend::app::formatRate(view.capacity.everyFrameBytesPerSecond)))
                       .arg(q(backend::app::formatRate(view.capacity.floorBytesPerSecond)))
                       .arg(in.invalidSamplingRate);
    } else {
        t.facts << QStringLiteral("Rate unknown (set target_fps in the profile)");
    }
    t.facts << QStringLiteral("Buffer %1 (%2)").arg(q(plan.summary())).arg(q(backend::processing::toString(plan.source)));
    t.facts << (in.diskBytesPerSecond > 0
                    ? QStringLiteral("Destination %1 (%2)").arg(q(backend::app::formatRate(in.diskBytesPerSecond))).arg(q(in.diskRateNote))
                    : QStringLiteral("Destination rate unknown (%1)").arg(q(in.diskRateNote)));

    QString seconds = plan.requestedFps > 0 ? QStringLiteral(", %1 s").arg(plan.seconds, 0, 'f', 2) : QString();
    t.summary = QStringLiteral("Buffer %1%2 - %3")
                    .arg(q(backend::processing::formatBufferBytes(plan.maxBytes)))
                    .arg(seconds)
                    .arg(t.status);
    t.shortSummary = QStringLiteral("Buffer %1%2: %3")
                         .arg(q(backend::processing::formatBufferBytes(plan.maxBytes)))
                         .arg(seconds)
                         .arg(t.status);
    if (worst >= 2) {
        QStringList ids;
        for (const auto* g : gates)
            if (severity(g->status) >= 2) ids << q(g->id);
        t.summary += QStringLiteral(": ") + ids.join(QStringLiteral(", "));
        t.shortSummary += QStringLiteral(" (%1)").arg(ids.size());
    }
    return t;
}

RecordingCapacityPanel::RecordingCapacityPanel(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("recordingCapacityPanel"));
    setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(2);
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("recordingCapacityTitle"));
    body_ = new QLabel(this);
    body_->setObjectName(QStringLiteral("recordingCapacityBody"));
    body_->setWordWrap(true);
    body_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title_);
    layout->addWidget(body_);
    setView({});
}

void RecordingCapacityPanel::setView(const backend::app::ExperimentCoordinator::RecordingCapacityView& view)
{
    shown_ = describeRecordingCapacity(view);
    const QString colour = shown_.status == QStringLiteral("Blocked")   ? QStringLiteral("#b3261e")
                           : shown_.status == QStringLiteral("Warning") ? QStringLiteral("#8a5a00")
                           : shown_.status == QStringLiteral("OK")      ? QStringLiteral("#1b6e2a")
                                                                        : QStringLiteral("#555555");
    title_->setText(QStringLiteral("<b>Recording capacity: <span style=\"color:%1\">%2</span></b>")
                        .arg(colour, shown_.status.toHtmlEscaped()));
    QStringList html;
    for (const auto& f : shown_.facts) html << f.toHtmlEscaped();
    for (const auto& g : shown_.gates) {
        const bool blocked = g.startsWith(QStringLiteral("Blocked"));
        const bool warn = g.startsWith(QStringLiteral("Warning"));
        html << QStringLiteral("<span style=\"color:%1\">%2</span>")
                    .arg(blocked ? QStringLiteral("#b3261e") : warn ? QStringLiteral("#8a5a00") : QStringLiteral("#1b6e2a"),
                         g.toHtmlEscaped());
    }
    body_->setText(html.join(QStringLiteral("<br>")));
    setToolTip(QStringList(shown_.facts + shown_.gates).join(QLatin1Char('\n')));
}

} // namespace frontend
