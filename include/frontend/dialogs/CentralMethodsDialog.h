#pragma once

#include <QDialog>

#include <cstdint>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;
class QWidget;

namespace backend::profiles {
class ProfileRegistryWorker;
struct RegistryWorkerSnapshot;
} // namespace backend::profiles

namespace frontend {

// Central profile registry view (#398 M1): sign in / out, refresh, and the
// revisions cached for the signed-in (or last) user, each with its own central
// state. Read-only toward the instrument: nothing here selects or applies a
// method. All work goes through the backend ProfileRegistryWorker; the dialog
// only enqueues commands and renders its snapshots (polled while visible), so
// the GUI thread never waits on the network or the cache.
class CentralMethodsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CentralMethodsDialog(backend::profiles::ProfileRegistryWorker& registry,
                                  QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void poll();
    void render(const backend::profiles::RegistryWorkerSnapshot& snapshot);
    void signIn();

    backend::profiles::ProfileRegistryWorker& registry_;
    QTimer* pollTimer_{nullptr};
    QLabel* status_{nullptr};
    QLabel* account_{nullptr};
    QLabel* activity_{nullptr};
    QLabel* warning_{nullptr};
    QWidget* signInRow_{nullptr};
    QLineEdit* email_{nullptr};
    QLineEdit* password_{nullptr};
    QPushButton* signInBtn_{nullptr};
    QPushButton* refreshBtn_{nullptr};
    QPushButton* cancelBtn_{nullptr};
    QPushButton* signOutBtn_{nullptr};
    QTableWidget* table_{nullptr};
    std::uint64_t renderedGeneration_{0};
    bool renderedBusy_{false};
    bool rendered_{false};
};

} // namespace frontend
