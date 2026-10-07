#pragma once

#include <QDialog>
#include <QPointer>
#include <QVector>

#include "frontend/utils/UpdateCatalog.h"

class QComboBox;
class QListWidget;
class QLabel;
class QTextBrowser;
class QPushButton;

namespace frontend {

class AutoUpdater;

// Lets the user pick an update channel (stable/beta) and install a specific
// version from that channel's history (including rollback). Reads the version
// list via AutoUpdater::fetchVersionIndex and installs via
// AutoUpdater::installVersion.
class SoftwareUpdatesDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SoftwareUpdatesDialog(AutoUpdater* updater, QWidget* parent = nullptr);

private slots:
    void reload();
    void onIndexReady(const QVector<updatecatalog::VersionEntry>& versions);
    void onIndexFailed(const QString& error);
    void installSelected();
    void openSelectedNotes();
    void updateButtons();

private:
    int selectedEntryIndex() const;

    QPointer<AutoUpdater> updater_;
    QComboBox* channelBox_{nullptr};
    QListWidget* list_{nullptr};
    QTextBrowser* notes_{nullptr};
    QLabel* status_{nullptr};
    QPushButton* installBtn_{nullptr};
    QPushButton* notesBtn_{nullptr};
    QVector<updatecatalog::VersionEntry> entries_;
};

} // namespace frontend
