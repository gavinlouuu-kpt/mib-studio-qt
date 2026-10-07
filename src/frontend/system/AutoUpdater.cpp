#include "frontend/system/AutoUpdater.h"

#include <algorithm>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
constexpr int kDefaultTimeoutMs = 6000;

QUrl latestUrlForChannel(const QString& channel) {
    return QUrl(QStringLiteral("https://updates.yofo.bio/%1/latest.json").arg(channel));
}

QVersionNumber currentAppVersion() {
    const auto v = QVersionNumber::fromString(QCoreApplication::applicationVersion());
    return v;
}

QByteArray normalizedHexLower(const QString& hex) {
    QByteArray out = hex.trimmed().toUtf8();
    for (auto& c : out) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

QString humanBytes(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("unknown");
    const double b = static_cast<double>(bytes);
    const char* units[] = {"B", "KB", "MB", "GB"};
    int idx = 0;
    double val = b;
    while (val >= 1024.0 && idx < 3) {
        val /= 1024.0;
        ++idx;
    }
    return QStringLiteral("%1 %2").arg(QString::number(val, 'f', idx == 0 ? 0 : 1), QString::fromLatin1(units[idx]));
}
} // namespace

namespace frontend {

AutoUpdater::AutoUpdater(QWidget* uiParent, QObject* parent)
    : QObject(parent), uiParent_(uiParent) {
    net_ = new QNetworkAccessManager(this);
}

QUrl AutoUpdater::manifestUrlFromEnvOrDefault() const {
    const QString override = qEnvironmentVariable("MIB_STUDIO_UPDATE_MANIFEST_URL");
    if (!override.trimmed().isEmpty()) {
        const QUrl url(override.trimmed());
        if (url.isValid() && (url.scheme() == "https" || url.scheme() == "http")) {
            return url;
        }
        SPDLOG_WARN("AutoUpdater: ignoring invalid MIB_STUDIO_UPDATE_MANIFEST_URL='{}'", override.toStdString());
    }
    return latestUrlForChannel(channel());
}

QString AutoUpdater::sanitizeChannel(const QString& c) {
    return (c.trimmed().toLower() == "beta") ? QStringLiteral("beta") : QStringLiteral("stable");
}

QString AutoUpdater::channel() const {
    QSettings s;
    // Default to the channel this build belongs to (derived from its own full
    // version), so a beta build opens on the beta channel and marks its own
    // release "current"; an explicit user choice (persisted) still wins.
    const QString buildChannel = updatecatalog::channelForVersion(currentVersion());
    return sanitizeChannel(s.value(QStringLiteral("Update/Channel"), buildChannel).toString());
}

void AutoUpdater::setChannel(const QString& c) {
    QSettings s;
    s.setValue(QStringLiteral("Update/Channel"), sanitizeChannel(c));
}

QString AutoUpdater::currentVersion() const {
    return QCoreApplication::applicationVersion();
}

QUrl AutoUpdater::indexUrlForChannel(const QString& c) const {
    return QUrl(QStringLiteral("https://updates.yofo.bio/%1/index.json").arg(sanitizeChannel(c)));
}

void AutoUpdater::fetchVersionIndex() {
    const QUrl url = indexUrlForChannel(channel());
    SPDLOG_INFO("AutoUpdater: fetching version index from {}", url.toString().toStdString());
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = net_->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit versionIndexFailed(reply->errorString());
            return;
        }
        const auto res = updatecatalog::parseIndex(reply->readAll());
        if (!res.ok) {
            emit versionIndexFailed(res.error);
            return;
        }
        emit versionIndexReady(res.versions);
    });
}

void AutoUpdater::installVersion(const updatecatalog::VersionEntry& e) {
    Manifest m;
    m.versionString = e.version;
    m.installerUrl = QUrl(e.installerUrl);
    m.installerSha256Hex = e.installerSha256Hex.toUtf8();
    m.installerSizeBytes = e.installerSizeBytes;
    m.releaseNotesUrl = QUrl(e.releaseNotesUrl);
    m.releaseNotes = e.releaseNotes;
    startInstallerDownload(m, /*interactive=*/true);
}

void AutoUpdater::infoBox(const QString& title, const QString& msg, bool interactive) const {
    if (!interactive) return;
    QMessageBox::information(uiParent_, title, msg);
}

void AutoUpdater::errorBox(const QString& title, const QString& msg, bool interactive) const {
    if (!interactive) {
        SPDLOG_WARN("AutoUpdater: {} - {}", title.toStdString(), msg.toStdString());
        return;
    }
    QMessageBox::warning(uiParent_, title, msg);
}

void AutoUpdater::checkForUpdates(bool interactive) {
    if (busy_) {
        if (interactive) {
            infoBox(QStringLiteral("Check for Updates"), QStringLiteral("An update check is already in progress."), true);
        }
        return;
    }
    busy_ = true;

    const QUrl manifestUrl = manifestUrlFromEnvOrDefault();
    if (!manifestUrl.isValid()) {
        errorBox(QStringLiteral("Check for Updates"),
                 QStringLiteral("Update manifest URL is invalid."),
                 interactive);
        busy_ = false;
        return;
    }

    startManifestRequest(manifestUrl, interactive);
}

void AutoUpdater::startManifestRequest(const QUrl& url, bool interactive) {
    SPDLOG_INFO("AutoUpdater: fetching manifest from {}", url.toString().toStdString());

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(kDefaultTimeoutMs);

    QNetworkReply* reply = net_->get(req);

    QProgressDialog* progress = nullptr;
    if (interactive) {
        progress = new QProgressDialog(QStringLiteral("Checking for updates..."), QStringLiteral("Cancel"), 0, 0, uiParent_);
        progress->setWindowModality(Qt::ApplicationModal);
        progress->setMinimumDuration(0);
        QObject::connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    }

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, progress, interactive]() {
        if (progress) {
            progress->hide();
            progress->deleteLater();
        }

        const QNetworkReply::NetworkError err = reply->error();
        const QByteArray body = reply->readAll();
        const QString errStr = reply->errorString();
        const int httpStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        if (err != QNetworkReply::NoError) {
            QString detailedError = errStr;
            if (httpStatusCode > 0) {
                detailedError += QStringLiteral("\nHTTP Status: %1").arg(httpStatusCode);
            }
            if (!body.isEmpty()) {
                const QString bodyStr = QString::fromUtf8(body);
                if (bodyStr.length() < 500) {  // Only include if reasonable length
                    detailedError += QStringLiteral("\nServer response: %1").arg(bodyStr);
                } else {
                    detailedError += QStringLiteral("\nServer response: %1...").arg(bodyStr.left(500));
                }
            }
            SPDLOG_ERROR("AutoUpdater: network error - {} (HTTP {})", errStr.toStdString(), httpStatusCode);
            if (!body.isEmpty()) {
                SPDLOG_ERROR("AutoUpdater: server response body: {}", QString::fromUtf8(body).toStdString());
            }
            errorBox(QStringLiteral("Check for Updates"),
                     QStringLiteral("Failed to check for updates.\n\n%1").arg(detailedError),
                     interactive);
            busy_ = false;
            return;
        }

        QString parseErr;
        const auto manifestOpt = parseManifest(body, &parseErr);
        if (!manifestOpt.has_value()) {
            errorBox(QStringLiteral("Check for Updates"),
                     QStringLiteral("Invalid update manifest.\n\n%1").arg(parseErr),
                     interactive);
            busy_ = false;
            return;
        }

        const Manifest manifest = *manifestOpt;
        const QVersionNumber current = currentAppVersion();
        const QVersionNumber available = QVersionNumber::fromString(manifest.versionString);

        if (available.isNull()) {
            errorBox(QStringLiteral("Check for Updates"),
                     QStringLiteral("Manifest version '%1' is not a valid version.").arg(manifest.versionString),
                     interactive);
            busy_ = false;
            return;
        }

        if (!current.isNull() && QVersionNumber::compare(available, current) <= 0) {
            SPDLOG_INFO("AutoUpdater: no update available (current={}, available={})",
                        QCoreApplication::applicationVersion().toStdString(),
                        manifest.versionString.toStdString());
            infoBox(QStringLiteral("Check for Updates"),
                    QStringLiteral("You are up to date.\n\nInstalled: %1\nAvailable: %2")
                        .arg(QCoreApplication::applicationVersion(), manifest.versionString),
                    interactive);
            busy_ = false;
            return;
        }

        if (!confirmUpdate(manifest)) {
            busy_ = false;
            return;
        }

        startInstallerDownload(manifest, true /* interactive */);
    });
}

bool AutoUpdater::confirmUpdate(const Manifest& manifest) {
    QDialog dialog(uiParent_);
    dialog.setWindowTitle(tr("Update Available"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* summary =
        new QLabel(tr("A new version is available.\n\nInstalled: %1\nAvailable: %2\n\nInstaller "
                      "size: %3\n\nInstall now?")
                       .arg(QCoreApplication::applicationVersion(), manifest.versionString,
                            humanBytes(manifest.installerSizeBytes)),
                   &dialog);
    summary->setTextFormat(Qt::PlainText);
    layout->addWidget(summary);
    if (!manifest.releaseNotes.isEmpty()) {
        auto* notes = new QTextBrowser(&dialog);
        notes->setMarkdown(manifest.releaseNotes);
        notes->setOpenExternalLinks(true);
        notes->setMinimumSize(560, 260);
        layout->addWidget(notes);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto* install = buttons->addButton(tr("Install"), QDialogButtonBox::AcceptRole);
    install->setDefault(true);
    if (manifest.releaseNotesUrl.isValid()) {
        auto* notes = buttons->addButton(tr("Release Notes"), QDialogButtonBox::ActionRole);
        connect(notes, &QPushButton::clicked, &dialog,
                [manifest]() { QDesktopServices::openUrl(manifest.releaseNotesUrl); });
    }
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}

std::optional<AutoUpdater::Manifest> AutoUpdater::parseManifest(const QByteArray& jsonBytes, QString* errorOut) const {
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("JSON parse error: %1").arg(perr.errorString());
        }
        return std::nullopt;
    }

    const QJsonObject obj = doc.object();
    const QString version = obj.value(QStringLiteral("version")).toString();
    const QString installerUrlStr = obj.value(QStringLiteral("installer_url")).toString();
    const QString sha256Str = obj.value(QStringLiteral("installer_sha256")).toString();
    const QJsonValue sizeVal = obj.value(QStringLiteral("installer_size_bytes"));
    const QString notesUrlStr = obj.value(QStringLiteral("release_notes_url")).toString();

    if (version.trimmed().isEmpty() || installerUrlStr.trimmed().isEmpty() || sha256Str.trimmed().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Missing required fields: version, installer_url, installer_sha256.");
        }
        return std::nullopt;
    }

    const QUrl installerUrl(installerUrlStr.trimmed());
    if (!installerUrl.isValid() || (installerUrl.scheme() != "https" && installerUrl.scheme() != "http")) {
        if (errorOut) {
            *errorOut = QStringLiteral("installer_url is not a valid http(s) URL.");
        }
        return std::nullopt;
    }

    qint64 sizeBytes = -1;
    if (sizeVal.isDouble()) {
        sizeBytes = static_cast<qint64>(sizeVal.toDouble(-1));
    }

    Manifest m;
    m.releaseNotes = obj.value(QStringLiteral("release_notes")).toString().left(16384);
    m.versionString = version.trimmed();
    m.installerUrl = installerUrl;
    m.installerSha256Hex = normalizedHexLower(sha256Str);
    m.installerSizeBytes = sizeBytes;
    if (!notesUrlStr.trimmed().isEmpty()) {
        m.releaseNotesUrl = QUrl(notesUrlStr.trimmed());
    }
    return m;
}

void AutoUpdater::startInstallerDownload(const Manifest& manifest, bool interactive) {
    SPDLOG_INFO("AutoUpdater: downloading installer from {}", manifest.installerUrl.toString().toStdString());

    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty()) {
        errorBox(QStringLiteral("Update"), QStringLiteral("Unable to locate a temporary folder."), interactive);
        busy_ = false;
        return;
    }

    const QString safeVersion = manifest.versionString;
    const QString fileName = QStringLiteral("MIB_Studio_Qt_Setup_v%1_%2.exe")
                                 .arg(safeVersion,
                                      QString::number(QDateTime::currentMSecsSinceEpoch()));
    const QString outPath = QDir(tempDir).filePath(fileName);

    QFile* outFile = new QFile(outPath, this);
    if (!outFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorBox(QStringLiteral("Update"),
                 QStringLiteral("Unable to create download file:\n%1").arg(outPath),
                 interactive);
        outFile->deleteLater();
        busy_ = false;
        return;
    }

    QNetworkRequest req(manifest.installerUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(10 * 60 * 1000); // installers can be large

    QNetworkReply* reply = net_->get(req);

    auto* progress = new QProgressDialog(QStringLiteral("Downloading update..."), QStringLiteral("Cancel"), 0, 100, uiParent_);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);

    QObject::connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::readyRead, this, [reply, outFile]() {
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            outFile->write(chunk);
        }
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, this, [progress](qint64 received, qint64 total) {
        if (total > 0) {
            const int pct = static_cast<int>((received * 100) / total);
            progress->setValue(std::clamp(pct, 0, 100));
        } else {
            progress->setRange(0, 0); // unknown size
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, outFile, progress, manifest, outPath, interactive]() {
        progress->hide();
        progress->deleteLater();

        // Flush pending bytes (if any were buffered but readyRead didn't run after final chunk)
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            outFile->write(tail);
        }
        outFile->flush();
        outFile->close();

        const QNetworkReply::NetworkError err = reply->error();
        const QString errStr = reply->errorString();
        reply->deleteLater();
        outFile->deleteLater();

        if (err != QNetworkReply::NoError) {
            QFile::remove(outPath);
            errorBox(QStringLiteral("Update"),
                     QStringLiteral("Failed to download update.\n\n%1").arg(errStr),
                     true);
            busy_ = false;
            return;
        }

        QString verifyErr;
        if (!verifyDownloadedInstaller(outPath, manifest, &verifyErr)) {
            QFile::remove(outPath);
            errorBox(QStringLiteral("Update"),
                     QStringLiteral("Downloaded installer failed verification.\n\n%1").arg(verifyErr),
                     true);
            busy_ = false;
            return;
        }

        QString launchErr;
        if (!launchInstallerElevated(outPath, &launchErr)) {
            errorBox(QStringLiteral("Update"),
                     QStringLiteral("Failed to launch the installer.\n\n%1").arg(launchErr),
                     true);
            busy_ = false;
            return;
        }

        QMessageBox::information(uiParent_,
                                 QStringLiteral("Update"),
                                 QStringLiteral("Installer launched. The application will now close."));
        QCoreApplication::quit();
    });
}

bool AutoUpdater::verifyDownloadedInstaller(const QString& path, const Manifest& manifest, QString* errorOut) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("Unable to open downloaded file for verification.");
        return false;
    }

    if (manifest.installerSizeBytes > 0) {
        const qint64 actualSize = f.size();
        if (actualSize != manifest.installerSizeBytes) {
            if (errorOut) {
                *errorOut = QStringLiteral("Size mismatch. Expected %1 bytes, got %2 bytes.")
                                .arg(QString::number(manifest.installerSizeBytes),
                                     QString::number(actualSize));
            }
            return false;
        }
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    QByteArray buf;
    buf.resize(1024 * 1024);
    while (!f.atEnd()) {
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0) break;
        if (n > 0) hasher.addData(buf.constData(), static_cast<int>(n));
    }

    const QByteArray actualHex = hasher.result().toHex();
    const QByteArray expectedHex = manifest.installerSha256Hex;
    if (expectedHex.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Manifest SHA-256 is empty.");
        return false;
    }

    if (actualHex != expectedHex) {
        if (errorOut) {
            *errorOut = QStringLiteral("SHA-256 mismatch.\nExpected: %1\nActual:   %2")
                            .arg(QString::fromLatin1(expectedHex),
                                 QString::fromLatin1(actualHex));
        }
        return false;
    }

    SPDLOG_INFO("AutoUpdater: installer verification OK (sha256={})", QString::fromLatin1(actualHex).toStdString());
    return true;
}

bool AutoUpdater::launchInstallerElevated(const QString& installerPath, QString* errorOut) const {
#ifdef _WIN32
    const std::wstring file = QDir::toNativeSeparators(installerPath).toStdWString();
    const std::wstring params = L"/SILENT /NORESTART";

    HINSTANCE res = ShellExecuteW(nullptr, L"runas", file.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<INT_PTR>(res);
    if (code <= 32) {
        if (errorOut) {
            *errorOut = QStringLiteral("ShellExecuteW failed (code=%1).").arg(QString::number(code));
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(installerPath);
    if (errorOut) *errorOut = QStringLiteral("Auto-update installer launch is only implemented on Windows.");
    return false;
#endif
}

} // namespace frontend
