#pragma once

#include <QStringList>
#include <QDialog>
#include <QVector>

#include "frontend/utils/ProcessingCoreCatalog.h"

class QComboBox;
class QLabel;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;
class QPushButton;

namespace backend { class AppBackend; }

namespace frontend {

class ProcessingCoreDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ProcessingCoreDialog(backend::AppBackend& backend, QWidget* parent = nullptr);

    // Called once during desktop startup, before capture/realtime begins.
    static bool restorePersistedCore(backend::AppBackend& backend, QString* error = nullptr);

private slots:
    void reload();
    void prepareAndActivateSelected();
    void updateButtons();

private:
    int selectedVersionIndex() const;
    // Registry trees, fetched in order: the subtract-ring tree (required),
    // then each native-only core line (optional: absent on a channel = no cores yet).
    void fetchTree(int tree);
    void loadTreeActive(int tree, processingcorecatalog::ParseResult index);
    void finishReload(const QString& note = {});
    QNetworkReply* startRegistryGet(const QUrl& url);
    // The loaded core, with its line resolved from the persisted selection by artifact.
    processingcorecatalog::ActiveCore activeCore() const;
    void updateActiveCoreLabel();
    void populate();
    void setBusy(bool busy, const QString& message = {});
    void downloadAndActivate(
        const processingcorecatalog::VersionEntry& version,
        const processingcorecatalog::NativePluginEntry& plugin,
        const QByteArray& manifestSha256Hex);

    backend::AppBackend& backend_;
    QNetworkAccessManager* network_{nullptr};
    QComboBox* channelBox_{nullptr};
    QListWidget* versions_{nullptr};
    QLabel* activeLabel_{nullptr};
    QLabel* statusLabel_{nullptr};
    QPushButton* prepareButton_{nullptr};
    QPushButton* refreshButton_{nullptr};
    QVector<processingcorecatalog::ParseResult> trees_;
    QVector<processingcorecatalog::CoreOption> options_;
    QStringList notes_;  // registry warnings collected during one reload
    bool busy_{false};
};

} // namespace frontend
