#include "frontend/widgets/RunStatusWidget.h"

#include "frontend/utils/ElidingLabel.h"

#include <QHBoxLayout>
#include <QLabel>

namespace frontend {

RunStatusWidget::RunStatusWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("runStatusWidget"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(4);
    glyph_ = new QLabel(this);
    glyph_->setObjectName(QStringLiteral("runStatusGlyph"));
    text_ = new ElidingLabel(this);
    text_->setObjectName(QStringLiteral("runStatusText"));
    text_->setElideMode(Qt::ElideRight);
    text_->setMinimumVisibleCharacters(8);
    text_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    text_->setMaximumWidth(260);
    layout->addWidget(glyph_);
    layout->addWidget(text_);
    setState(RunPresentationState{});
}

void RunStatusWidget::bind(RunStatusModel* model)
{
    if (!model) return;
    connect(model, &RunStatusModel::changed, this, &RunStatusWidget::setState);
    setState(model->state());
}

QString RunStatusWidget::text() const { return state_.text(); }

void RunStatusWidget::setState(const RunPresentationState& state)
{
    state_ = state;
    glyph_->setText(runPhaseGlyph(state.phase));
    const QString label = state.text();
    text_->setText(label);
    // Style is a secondary cue; the text already carries the state.
    QString color;
    switch (state.phase) {
    case RunPhase::Running: color = QStringLiteral("#1b7f1b"); break;
    case RunPhase::Failed: color = QStringLiteral("#b00020"); break;
    case RunPhase::Saving:
    case RunPhase::Stopping:
    case RunPhase::Starting: color = QStringLiteral("#b06a00"); break;
    default: color.clear(); break;
    }
    glyph_->setStyleSheet(color.isEmpty() ? QString() : QStringLiteral("color: %1; font-weight: bold;").arg(color));
    setToolTip(tr("Run state: %1 (%2)").arg(label, QLatin1String(toString(state.phase))));
    setAccessibleName(tr("Run state: %1").arg(label));
}

} // namespace frontend
