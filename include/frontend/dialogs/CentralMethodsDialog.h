#pragma once

#include <QDialog>

#include "backend/profiles/ProfileRegistry.h" // MethodContext

#include <cstdint>
#include <functional>
#include <string>

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

// What the dialog needs from the shell for #398 M2b. Every hook is optional:
// without it the matching action is disabled (with a tooltip), which keeps
// the dialog usable read-only and easy to test.
struct CentralMethodsHooks {
    // This instrument + current processing core / camera source.
    std::function<backend::profiles::MethodContext()> methodContext;
    std::string instrumentName;
    // The config.json text currently applied (AppBackend::getLastConfigJson).
    std::function<std::string()> currentConfigJson;
    // Replace config.json with the method's exact bytes (AppConfigWatcher::
    // applyMethodDocument). Returns "" on success, else the reason.
    std::function<QString(const QByteArray& configText, QString* backupPath)> applyConfig;
    // "Mark validated" (AppBackend::requestMethodValidation): job id (0 =
    // refused) and the reason.
    struct ValidationOutcome {
        std::uint64_t jobId{0};
        std::string error;
    };
    std::function<ValidationOutcome(const std::string& revisionId, const std::string& evidenceFile,
                                    bool passed)>
        recordValidation;
    // Prompts; default to QFileDialog / QMessageBox. Tests replace them.
    std::function<QString(const QString& title)> pickEvidenceFile;
    std::function<bool(const QString& title, const QString& text)> confirm;
};

// Central profile registry view (#398 M1/M2b): sign in / out, refresh, and the
// revisions cached for the signed-in (or last) user, each with its own central
// state and its local validation on this instrument. Select a row to Apply it
// (materialize if needed, show what changes, confirm, back up config.json,
// apply exactly) or to record a local validation from a test-run file. All
// registry work goes through the backend ProfileRegistryWorker; the dialog
// only enqueues commands and renders its snapshots (polled while visible), so
// the GUI thread never waits on the network or the cache.
class CentralMethodsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit CentralMethodsDialog(backend::profiles::ProfileRegistryWorker& registry,
                                  CentralMethodsHooks hooks = {}, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void poll();
    void render(const backend::profiles::RegistryWorkerSnapshot& snapshot);
    void signIn();
    QString selectedRevision() const;
    void updateActions(const backend::profiles::RegistryWorkerSnapshot& snapshot);
    void apply();
    void continueApply(const std::string& revisionId);
    void markValidated(bool passed);
    void setNotice(const QString& text, bool error);

    backend::profiles::ProfileRegistryWorker& registry_;
    CentralMethodsHooks hooks_;
    QTimer* pollTimer_{nullptr};
    QLabel* instrument_{nullptr};
    QLabel* notice_{nullptr};
    QPushButton* applyBtn_{nullptr};
    QPushButton* validateBtn_{nullptr};
    QPushButton* failedBtn_{nullptr};
    // Apply waiting for its Materialize job.
    std::uint64_t pendingApplyJob_{0};
    std::string pendingApplyRevision_;
    // Re-render triggers that do not bump the registry generation.
    std::string renderedConfig_;
    std::string renderedContext_;
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
