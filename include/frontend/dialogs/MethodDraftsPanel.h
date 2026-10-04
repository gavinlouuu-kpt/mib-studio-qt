#pragma once
// Drafts tab of the Central Methods dialog (#398 M3b): the signed-in user's
// local method drafts with their base revision and status, and the authoring
// actions on the selected draft — new method from the current config.json,
// edit release notes, submit for review, and the explicit conflict choices
// (submit as branch, new draft from the head, discard). Drafts are local
// until Submit; a stale base never rebases silently. All work is queued on the
// backend ProfileRegistryWorker; the panel only renders its snapshots.
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;

namespace backend::profiles {
class ProfileRegistryWorker;
struct RegistryWorkerSnapshot;
} // namespace backend::profiles

namespace frontend {

struct CentralMethodsHooks;

class MethodDraftsPanel final : public QWidget {
    Q_OBJECT
public:
    // `hooks` and `notice` must outlive the panel (owned by the dialog).
    MethodDraftsPanel(backend::profiles::ProfileRegistryWorker& registry, const CentralMethodsHooks& hooks,
                      std::function<void(const QString&, bool)> notice, QWidget* parent = nullptr);
    void render(const backend::profiles::RegistryWorkerSnapshot& snapshot);

private:
    QString selectedDraft() const;
    void updateActions(const backend::profiles::RegistryWorkerSnapshot& snapshot);
    void newMethod();
    void editNotes();
    void submit(bool asBranch);
    void draftFromHead();
    void discard();

    backend::profiles::ProfileRegistryWorker& registry_;
    const CentralMethodsHooks& hooks_;
    std::function<void(const QString&, bool)> notice_;
    QTableWidget* table_{nullptr};
    QLabel* conflict_{nullptr};
    QPushButton* newMethodBtn_{nullptr};
    QPushButton* notesBtn_{nullptr};
    QPushButton* submitBtn_{nullptr};
    QPushButton* branchBtn_{nullptr};
    QPushButton* fromHeadBtn_{nullptr};
    QPushButton* discardBtn_{nullptr};
};

} // namespace frontend
