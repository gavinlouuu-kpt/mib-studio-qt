// Compact run lifecycle presentation (issue #363): glyph + text, never color
// alone, bounded width (elided detail), full text in tooltip/accessible name.
#pragma once

#include "frontend/models/RunStatusModel.h"

#include <QWidget>

class QLabel;

namespace frontend {

class ElidingLabel;

class RunStatusWidget : public QWidget {
    Q_OBJECT
public:
    explicit RunStatusWidget(QWidget* parent = nullptr);
    void bind(RunStatusModel* model);
    RunPhase phase() const { return state_.phase; }
    QString text() const;

public slots:
    void setState(const frontend::RunPresentationState& state);

private:
    RunPresentationState state_;
    QLabel* glyph_ = nullptr;
    ElidingLabel* text_ = nullptr;
};

} // namespace frontend
