// Persistent, wrapping alert surface (issue #363).
//
// Shows the highest-severity unacknowledged alert from a UiAlertModel with
// its remediation, a count of further unacknowledged alerts, an expandable
// bounded details list, and an Acknowledge action. Acknowledging hides the
// banner; it never resolves the underlying condition (the owner does).
// Metrics refreshes cannot touch it.
#pragma once

#include "frontend/models/RunStatusModel.h"

#include <QFrame>

class QLabel;
class QPlainTextEdit;
class QToolButton;

namespace frontend {

class AlertBanner : public QFrame {
    Q_OBJECT
public:
    explicit AlertBanner(QWidget* parent = nullptr);
    void bind(UiAlertModel* model);
    QString summaryText() const;
    bool detailsVisible() const;
    QToolButton* acknowledgeButton() const { return ackBtn_; }
    QToolButton* detailsButton() const { return detailsBtn_; }

public slots:
    void refresh();

private:
    UiAlertModel* model_ = nullptr;
    QLabel* glyph_ = nullptr;
    QLabel* summary_ = nullptr;
    QToolButton* detailsBtn_ = nullptr;
    QToolButton* ackBtn_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
};

} // namespace frontend
