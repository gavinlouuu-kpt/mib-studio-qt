#include "frontend/utils/ElidingLabel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

namespace frontend {

ElidingLabel::ElidingLabel(QWidget* parent) : QLabel(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
}

ElidingLabel::ElidingLabel(const QString& text, QWidget* parent) : ElidingLabel(parent)
{
    setText(text);
}

void ElidingLabel::setText(const QString& text)
{
    QLabel::setText(text);
    setToolTip(text);
    setAccessibleDescription(text);
    update();
}

void ElidingLabel::setElideMode(Qt::TextElideMode mode)
{
    elideMode_ = mode;
    update();
}

void ElidingLabel::setMinimumVisibleCharacters(int chars)
{
    minimumVisibleCharacters_ = qMax(1, chars);
    updateGeometry();
}

QSize ElidingLabel::minimumSizeHint() const
{
    // Independent of the text: a handful of characters plus the ellipsis.
    const QFontMetrics fm(font());
    const int w = fm.horizontalAdvance(QStringLiteral("M")) * minimumVisibleCharacters_ +
                  fm.horizontalAdvance(QStringLiteral("…"));
    const QMargins m = contentsMargins();
    return QSize(w + m.left() + m.right(), fm.height() + m.top() + m.bottom());
}

QSize ElidingLabel::sizeHint() const
{
    // Preferred: the full text, but bounded so a very long value cannot make
    // a layout prefer an unreasonable width.
    const QFontMetrics fm(font());
    const int full = fm.horizontalAdvance(fullText());
    const int bounded = qMin(full, fm.horizontalAdvance(QStringLiteral("M")) * 60);
    const QMargins m = contentsMargins();
    return QSize(qMax(bounded, minimumSizeHint().width()) + m.left() + m.right(),
                 fm.height() + m.top() + m.bottom());
}

void ElidingLabel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    QStyleOption opt;
    opt.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);
    const QRect r = contentsRect();
    const QFontMetrics fm(font());
    const QString shown = fm.elidedText(fullText(), elideMode_, r.width());
    elided_ = shown != fullText();
    painter.setFont(font());
    painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, foregroundRole()));
    painter.drawText(r, static_cast<int>(alignment()) | Qt::TextSingleLine, shown);
}

void ElidingLabel::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    QAction* copy = menu.addAction(tr("Copy full text"));
    if (menu.exec(event->globalPos()) == copy) {
        QApplication::clipboard()->setText(fullText());
    }
}

} // namespace frontend
