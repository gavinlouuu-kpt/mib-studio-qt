#pragma once

#include <optional>

#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QUrl>
#include <QVector>

#include "frontend/utils/UpdateCatalog.h"

class QWidget;
class QNetworkAccessManager;

namespace frontend {

class AutoUpdater final : public QObject {
    Q_OBJECT
public:
    explicit AutoUpdater(QWidget* uiParent, QObject* parent = nullptr);

    // If interactive=true, user-facing dialogs are shown even for "no update" / errors.
    // If interactive=false, only prompts when an update is available (quiet check).
    void checkForUpdates(bool interactive);

    // Selected update channel, persisted in QSettings ("Update/Channel").
    // Always one of "stable" | "beta"; defaults to "stable".
    QString channel() const;
    void setChannel(const QString& channel);

    // The running application version (QCoreApplication::applicationVersion()).
    QString currentVersion() const;

    // Asynchronously fetch the selected channel's index.json. Emits
    // versionIndexReady on success or versionIndexFailed on error.
    void fetchVersionIndex();

    // Download + verify + elevate-install a specific catalog entry (reuses the
    // same path as a normal update; always interactive).
    void installVersion(const updatecatalog::VersionEntry& entry);

signals:
    void versionIndexReady(const QVector<updatecatalog::VersionEntry>& versions);
    void versionIndexFailed(const QString& error);

private:
    static QString sanitizeChannel(const QString& channel);
    QUrl indexUrlForChannel(const QString& channel) const;

    struct Manifest {
        QString versionString;
        QUrl installerUrl;
        QByteArray installerSha256Hex; // hex bytes, lowercased
        qint64 installerSizeBytes{-1};
        QUrl releaseNotesUrl;
        QString releaseNotes;
    };

    QUrl manifestUrlFromEnvOrDefault() const;
    void startManifestRequest(const QUrl& url, bool interactive);
    std::optional<Manifest> parseManifest(const QByteArray& jsonBytes, QString* errorOut) const;
    bool confirmUpdate(const Manifest& manifest);
    void startInstallerDownload(const Manifest& manifest, bool interactive);

    bool verifyDownloadedInstaller(const QString& path, const Manifest& manifest, QString* errorOut) const;
    bool launchInstallerElevated(const QString& installerPath, QString* errorOut) const;

    void infoBox(const QString& title, const QString& msg, bool interactive) const;
    void errorBox(const QString& title, const QString& msg, bool interactive) const;

    QPointer<QWidget> uiParent_;
    QNetworkAccessManager* net_{nullptr};
    bool busy_{false};
};

} // namespace frontend

