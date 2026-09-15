// Bounded-width text label (issue #358).
//
// A QLabel whose *displayed* text is elided with the current font metrics
// while the full value stays available (`fullText()`, tooltip, accessible
// description, "Copy" context action). Its minimum size hint is independent
// of the text length, so a long path, profile name or status string can
// never widen the window it lives in.
#pragma once

#include <QLabel>
#include <QString>

namespace frontend {

class ElidingLabel : public QLabel {
    Q_OBJECT
public:
    explicit ElidingLabel(QWidget* parent = nullptr);
    explicit ElidingLabel(const QString& text, QWidget* parent = nullptr);

    // Full (unelided) value. `text()` returns the same string; only painting
    // is elided.
    void setText(const QString& text);
    QString fullText() const { return QLabel::text(); }

    void setElideMode(Qt::TextElideMode mode);
    Qt::TextElideMode elideMode() const { return elideMode_; }

    // Minimum number of characters that must stay readable (drives the
    // minimum size hint); default 8.
    void setMinimumVisibleCharacters(int chars);

    // True when the last paint had to elide the text.
    bool isElided() const { return elided_; }

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    Qt::TextElideMode elideMode_{Qt::ElideMiddle};
    int minimumVisibleCharacters_{8};
    mutable bool elided_{false};
};

} // namespace frontend
