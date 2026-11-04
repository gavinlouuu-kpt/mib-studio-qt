#pragma once

// Recording capacity for the run setup (#833): the camera window, the
// requested and maximum frame rate, the save stream, the experiment buffer
// sized for them (frames, size, seconds), the destination's write rate and the
// four capacity gates (camera.window, camera.rate, storage.buffer,
// storage.throughput) with their reason and fix. Shown in the Overview tab
// under the camera window controls; MainWindow shows the one-line summary next
// to Start. Status is text plus colour, never colour alone (#363).

#include "backend/app/ExperimentCoordinator.h"

#include <QFrame>
#include <QString>
#include <QStringList>

class QLabel;

namespace frontend {

struct RecordingCapacityText {
    QString status;    // "OK", "Warning", "Blocked" or "Unknown"
    QString summary;      // one line: buffer, status and the blocking gate ids
    QString shortSummary; // "Buffer 4.7 GiB, 2.00 s: OK" / ": Blocked (2)" (run bar)
    QStringList facts; // window, rate, save stream, buffer, destination
    QStringList gates; // "Blocked - camera.rate: reason -> remediation", worst first
};

RecordingCapacityText describeRecordingCapacity(const backend::app::ExperimentCoordinator::RecordingCapacityView& view);

class RecordingCapacityPanel : public QFrame {
    Q_OBJECT
public:
    explicit RecordingCapacityPanel(QWidget* parent = nullptr);
    void setView(const backend::app::ExperimentCoordinator::RecordingCapacityView& view);
    const RecordingCapacityText& shown() const { return shown_; }

private:
    QLabel* title_{nullptr};
    QLabel* body_{nullptr};
    RecordingCapacityText shown_;
};

} // namespace frontend
