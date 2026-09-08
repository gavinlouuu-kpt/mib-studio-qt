#include "frontend/widgets/AlertBanner.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace frontend {

namespace {
QString severityGlyph(AlertSeverity s)
{
    switch (s) {
    case AlertSeverity::Info: return QStringLiteral("ℹ");
    case AlertSeverity::Warning: return QStringLiteral("⚠");
    case AlertSeverity::Error: return QStringLiteral("✕");
    case AlertSeverity::Critical: return QStringLiteral("⛔");
    }
    return QStringLiteral("!");
}
QString severityWord(AlertSeverity s)
{
    switch (s) {
    case AlertSeverity::Info: return QObject::tr("Info");
    case AlertSeverity::Warning: return QObject::tr("Warning");
    case AlertSeverity::Error: return QObject::tr("Error");
    case AlertSeverity::Critical: return QObject::tr("Critical");
    }
    return QString();
}
} // namespace

AlertBanner::AlertBanner(QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("alertBanner"));
    setFrameShape(QFrame::StyledPanel);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 4, 8, 4);
    outer->setSpacing(2);
    auto* row = new QHBoxLayout();
    row->setSpacing(6);
    glyph_ = new QLabel(this);
    glyph_->setObjectName(QStringLiteral("alertGlyph"));
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("alertSummary"));
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::PlainText);
    summary_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsBtn_ = new QToolButton(this);
    detailsBtn_->setObjectName(QStringLiteral("alertDetailsBtn"));
    detailsBtn_->setText(tr("Details"));
    detailsBtn_->setCheckable(true);
    detailsBtn_->setFocusPolicy(Qt::StrongFocus);
    ackBtn_ = new QToolButton(this);
    ackBtn_->setObjectName(QStringLiteral("alertAcknowledgeBtn"));
    ackBtn_->setText(tr("Acknowledge"));
    ackBtn_->setToolTip(tr("Hide this alert. The underlying condition stays recorded until it is resolved."));
    ackBtn_->setFocusPolicy(Qt::StrongFocus);
    row->addWidget(glyph_);
    row->addWidget(summary_, 1);
    row->addWidget(detailsBtn_);
    row->addWidget(ackBtn_);
    outer->addLayout(row);
    details_ = new QPlainTextEdit(this);
    details_->setObjectName(QStringLiteral("alertDetails"));
    details_->setReadOnly(true);
    details_->setMaximumHeight(120);
    details_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    details_->setVisible(false);
    outer->addWidget(details_);
    connect(detailsBtn_, &QToolButton::toggled, this, [this](bool on) { details_->setVisible(on); updateGeometry(); });
    connect(ackBtn_, &QToolButton::clicked, this, [this]() { if (model_) model_->acknowledgeAll(); });
    setStyleSheet(QStringLiteral("#alertBanner { background: #fff4e5; border: 1px solid #e0a800; }"));
    hide();
}

void AlertBanner::bind(UiAlertModel* model)
{
    model_ = model;
    if (model_) connect(model_, &UiAlertModel::changed, this, &AlertBanner::refresh);
    refresh();
}

QString AlertBanner::summaryText() const { return summary_ ? summary_->text() : QString(); }

bool AlertBanner::detailsVisible() const { return details_ && details_->isVisible(); }

void AlertBanner::refresh()
{
    if (!model_) { hide(); return; }
    const UiAlert* head = model_->headline();
    if (!head) {
        hide();
        return;
    }
    const auto pending = model_->unacknowledged();
    QString text = QStringLiteral("%1: %2").arg(severityWord(head->severity), head->message);
    if (head->count > 1) text += tr(" (×%1)").arg(head->count);
    if (!head->remediation.isEmpty()) text += QStringLiteral(" — ") + head->remediation;
    if (pending.size() > 1) text += tr(" (+%1 more)").arg(pending.size() - 1);
    glyph_->setText(severityGlyph(head->severity));
    summary_->setText(text);
    setAccessibleName(tr("Alert: %1").arg(text));
    QString detailsText;
    for (const UiAlert& a : model_->unresolved()) {
        detailsText += QStringLiteral("[%1] %2 — %3 (×%4, last %5)%6%7\n")
                           .arg(severityWord(a.severity), a.key, a.message)
                           .arg(a.count)
                           .arg(QDateTime::fromMSecsSinceEpoch(a.lastAtMs).toString(QStringLiteral("HH:mm:ss")))
                           .arg(a.remediation.isEmpty() ? QString() : QStringLiteral(" — ") + a.remediation)
                           .arg(a.acknowledged ? tr(" [acknowledged]") : QString());
    }
    if (model_->overflowDropped() > 0) detailsText += tr("(%1 older alerts dropped from history)\n").arg(model_->overflowDropped());
    details_->setPlainText(detailsText);
    const bool critical = head->severity >= AlertSeverity::Error;
    setStyleSheet(critical ? QStringLiteral("#alertBanner { background: #fdecea; border: 1px solid #b00020; }")
                           : QStringLiteral("#alertBanner { background: #fff4e5; border: 1px solid #e0a800; }"));
    show();
}

} // namespace frontend
