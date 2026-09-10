#include "frontend/dialogs/ProcessingCoreDialog.h"

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingCoreCache.h"
#include "backend/processing/ProcessingCoreAbi.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/CaptureService.h"
#include "frontend/utils/ProcessingCoreSettings.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

#ifndef MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256
#define MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256 ""
#endif

namespace frontend {
namespace {

constexpr qint64 kMaxRegistryBytes = 2 * 1024 * 1024;
constexpr qint64 kMaxNetworkBufferBytes = 1024 * 1024;

bool isHttps(const QUrl& url) {
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           !url.host().isEmpty();
}

void installAbsoluteDeadline(QNetworkReply* reply, int milliseconds) {
    QTimer::singleShot(milliseconds, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });
}

void installDownloadLimit(QNetworkReply* reply, qint64 maximumBytes) {
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [reply, maximumBytes](qint64 received, qint64 total) {
                         if (received > maximumBytes || total > maximumBytes) reply->abort();
                     });
}

struct NativeDownloadState {
    std::shared_ptr<QTemporaryFile> file;
    qint64 written{0};
    QString error;
};

bool drainNativeDownload(QNetworkReply* reply,
                         const std::shared_ptr<NativeDownloadState>& state,
                         qint64 expectedBytes) {
    while (reply->bytesAvailable() > 0) {
        const QByteArray chunk = reply->read(std::min<qint64>(reply->bytesAvailable(), 64 * 1024));
        if (chunk.isEmpty()) break;
        if (state->written > expectedBytes - chunk.size()) {
            state->error = QObject::tr("Download exceeded the declared artifact size.");
            return false;
        }
        if (state->file->write(chunk) != chunk.size()) {
            state->error = QObject::tr("Could not stream the downloaded core to disk.");
            return false;
        }
        state->written += chunk.size();
    }
    return true;
}

QString registryBaseUrl() {
    const QString configured =
        qEnvironmentVariable("MIB_STUDIO_PROCESSING_CORE_BASE_URL").trimmed();
    return configured.isEmpty() ? QStringLiteral("https://updates.yofo.bio") : configured;
}

QString cacheRoot() {
    const QString configured =
        qEnvironmentVariable("MIB_STUDIO_PROCESSING_CORE_CACHE_DIR").trimmed();
    if (!configured.isEmpty()) return QDir(configured).absolutePath();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .absoluteFilePath(QStringLiteral("processing-cores"));
}

QString platformOs() {
#if defined(_WIN32)
    return QStringLiteral("windows");
#elif defined(__APPLE__)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

QString platformArch() {
    const QString architecture = QSysInfo::currentCpuArchitecture().toLower();
    if (architecture == QStringLiteral("amd64") || architecture == QStringLiteral("x64")) {
        return QStringLiteral("x86_64");
    }
    if (architecture == QStringLiteral("arm64")) return QStringLiteral("aarch64");
    return architecture;
}

processingcorecatalog::CompatibilityResult compatibilityFor(
    const processingcorecatalog::VersionEntry& version) {
    const auto host = backend::processing::bundledProcessingCoreIdentity();
    return processingcorecatalog::evaluateCompatibility(version, {
        platformOs(), platformArch(), QCoreApplication::applicationVersion(),
        static_cast<int>(MIB_PROCESSING_ENGINE_ABI_VERSION),
        static_cast<int>(MIB_PROCESSING_CONTRACT_VERSION),
        QString::fromStdString(host.runtimeFingerprint),
        qEnvironmentVariable("MIB_STUDIO_PROCESSING_CORE_VERSION").trimmed()});
}

std::function<bool(const std::filesystem::path&, std::string&)> trustVerifier(
    const processingcorecatalog::NativePluginEntry& plugin) {
#if !defined(NDEBUG)
    if (qEnvironmentVariableIntValue("MIB_STUDIO_ALLOW_UNSIGNED_PROCESSING_CORE") == 1) {
        return [](const std::filesystem::path&, std::string&) { return true; };
    }
#endif
    if (!plugin.signingRequired) {
        return [](const std::filesystem::path&, std::string& error) {
            error = "processing-core manifest does not require an artifact signature";
            return false;
        };
    }
    const std::string signingScheme = plugin.signingScheme.toStdString();
#if defined(_WIN32)
    if (plugin.signingScheme != QStringLiteral("authenticode")) {
        return [signingScheme](const std::filesystem::path&, std::string& error) {
            error = "unsupported Windows processing-core trust scheme: " + signingScheme;
            return false;
        };
    }
    std::string approved = MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256;
#if !defined(NDEBUG)
    const QString debugOverride =
        qEnvironmentVariable("MIB_STUDIO_PROCESSING_CORE_SIGNER_SPKI_SHA256").trimmed();
    if (!debugOverride.isEmpty()) approved = debugOverride.toStdString();
#endif
    return [approved](const std::filesystem::path& path, std::string& error) {
        return backend::processing::verifyProcessingCoreAuthenticode(path, approved, error);
    };
#elif defined(__linux__)
    if (plugin.signingScheme != QStringLiteral("ed25519")) {
        return [signingScheme](const std::filesystem::path&, std::string& error) {
            error = "unsupported Linux processing-core trust scheme: " + signingScheme;
            return false;
        };
    }
    backend::processing::ProcessingCoreDetachedSignature signature;
    signature.publicKeySpkiDerBase64 = plugin.signingPublicKeySpkiBase64.toStdString();
    signature.signatureBase64 = plugin.signingSignatureBase64.toStdString();
    std::string approved = MIB_PROCESSING_CORE_ED25519_SPKI_SHA256;
#if !defined(NDEBUG)
    const QString debugOverride =
        qEnvironmentVariable("MIB_STUDIO_PROCESSING_CORE_ED25519_SPKI_SHA256").trimmed();
    if (!debugOverride.isEmpty()) approved = debugOverride.toStdString();
#endif
    return [signature, approved](const std::filesystem::path& path, std::string& error) {
        return backend::processing::verifyProcessingCoreEd25519(path, signature, approved,
                                                                error);
    };
#else
    return [signingScheme](const std::filesystem::path&, std::string& error) {
        error = "processing-core trust scheme '" + signingScheme +
                "' is not implemented for this host platform";
        return false;
    };
#endif
}

backend::processing::ProcessingCoreLoadRequirements loadRequirements(
    const processingcorecatalog::VersionEntry& version,
    const processingcorecatalog::NativePluginEntry& plugin,
    const QByteArray& manifestSha256Hex = {}) {
    backend::processing::ProcessingCoreLoadRequirements requirements;
    requirements.expectedVersion = version.version.toStdString();
    requirements.expectedContractVersion = static_cast<uint32_t>(plugin.contractVersion);
    requirements.expectedEngineAbiVersion = static_cast<uint32_t>(plugin.engineAbiVersion);
    requirements.expectedRuntimeFingerprint = plugin.runtimeFingerprint.toStdString();
    requirements.artifactSha256 = plugin.sha256.toStdString();
    requirements.releaseTag = version.releaseTag.toStdString();
    requirements.manifestSha256 = manifestSha256Hex.toStdString();
    requirements.trustVerifier = trustVerifier(plugin);
    return requirements;
}

} // namespace

ProcessingCoreDialog::ProcessingCoreDialog(backend::AppBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend), network_(new QNetworkAccessManager(this)) {
    setWindowTitle(tr("Processing Core"));
    resize(620, 460);
    auto* root = new QVBoxLayout(this);
    activeLabel_ = new QLabel(this);
    activeLabel_->setWordWrap(true);
    root->addWidget(activeLabel_);

    auto* channelRow = new QHBoxLayout();
    channelRow->addWidget(new QLabel(tr("Registry channel:"), this));
    channelBox_ = new QComboBox(this);
    channelBox_->addItem(tr("Stable"), QStringLiteral("stable"));
    channelBox_->addItem(tr("Beta"), QStringLiteral("beta"));
    const QString savedChannel = QSettings().value(
        QStringLiteral("ProcessingCore/Channel"), QStringLiteral("stable")).toString();
    const int channelIndex = channelBox_->findData(savedChannel);
    if (channelIndex >= 0) channelBox_->setCurrentIndex(channelIndex);
    refreshButton_ = new QPushButton(tr("Refresh"), this);
    channelRow->addWidget(channelBox_, 1);
    channelRow->addWidget(refreshButton_);
    root->addLayout(channelRow);

    versions_ = new QListWidget(this);
    root->addWidget(versions_, 1);
    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    root->addWidget(statusLabel_);

    auto* buttons = new QHBoxLayout();
    prepareButton_ = new QPushButton(tr("Prepare && Activate"), this);
    auto* closeButton = new QPushButton(tr("Close"), this);
    buttons->addWidget(prepareButton_);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);
    root->addLayout(buttons);

    connect(refreshButton_, &QPushButton::clicked, this, &ProcessingCoreDialog::reload);
    connect(channelBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                QSettings().setValue(QStringLiteral("ProcessingCore/Channel"),
                                     channelBox_->currentData());
                reload();
            });
    connect(versions_, &QListWidget::itemSelectionChanged, this,
            &ProcessingCoreDialog::updateButtons);
    connect(prepareButton_, &QPushButton::clicked, this,
            &ProcessingCoreDialog::prepareAndActivateSelected);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    updateActiveCoreLabel();
    reload();
}

bool ProcessingCoreDialog::restorePersistedCore(backend::AppBackend& backend, QString* error) {
    QSettings settings;
    const QString version = settings.value(QStringLiteral("ProcessingCore/Version")).toString();
    const QString path = settings.value(QStringLiteral("ProcessingCore/Path")).toString();
    const QString sha = settings.value(QStringLiteral("ProcessingCore/Sha256")).toString();
    const QString hardPin =
        qEnvironmentVariable("MIB_STUDIO_PROCESSING_CORE_VERSION").trimmed();
    if (!hardPin.isEmpty() && version.isEmpty() &&
        backend.processing().activeProcessingCoreIdentity().version == hardPin.toStdString()) {
        return true;
    }
    if (!hardPin.isEmpty() && version != hardPin) {
        backend.processing().markProcessingCoreSelectionUnavailable();
        if (error) {
            *error = version.isEmpty()
                         ? tr("Administrator-pinned core %1 has not been prepared.").arg(hardPin)
                         : tr("Persisted core %1 does not satisfy administrator pin %2.")
                               .arg(version, hardPin);
        }
        return false;
    }
    const bool hasPersistedSelection = !version.isEmpty() || !path.isEmpty() || !sha.isEmpty();
    if (!hasPersistedSelection) return true;
    if (version.isEmpty() || path.isEmpty() || sha.isEmpty()) {
        backend.processing().markProcessingCoreSelectionUnavailable();
        if (error) *error = tr("Persisted processing-core selection is incomplete.");
        return false;
    }
    processingcorecatalog::NativePluginEntry persistedCompatibility;
    persistedCompatibility.appMinVersion =
        settings.value(QStringLiteral("ProcessingCore/AppMinVersion")).toString();
    persistedCompatibility.appMaxVersion =
        settings.value(QStringLiteral("ProcessingCore/AppMaxVersion")).toString();
    persistedCompatibility.signingScheme =
        settings.value(QStringLiteral("ProcessingCore/SigningScheme")).toString();
    persistedCompatibility.signingRequired =
        settings.value(QStringLiteral("ProcessingCore/SigningRequired"), false).toBool();
    persistedCompatibility.signingPublicKeySpkiBase64 =
        settings.value(QStringLiteral("ProcessingCore/SigningPublicKeySpkiBase64")).toString();
    persistedCompatibility.signingPublicKeySpkiSha256 =
        settings.value(QStringLiteral("ProcessingCore/SigningPublicKeySpkiSha256")).toString();
    persistedCompatibility.signingSignatureBase64 =
        settings.value(QStringLiteral("ProcessingCore/SigningSignatureBase64")).toString();
    if (persistedCompatibility.appMinVersion.isEmpty() ||
        !processingcorecatalog::isAppCompatible(
            persistedCompatibility, QCoreApplication::applicationVersion())) {
        backend.processing().markProcessingCoreSelectionUnavailable();
        if (error) *error = tr("Persisted processing core is incompatible with this app version.");
        return false;
    }

    backend::processing::ProcessingCoreLoadRequirements requirements;
    requirements.expectedVersion = version.toStdString();
    requirements.expectedContractVersion = static_cast<uint32_t>(
        settings.value(QStringLiteral("ProcessingCore/ContractVersion"), 1).toUInt());
    requirements.expectedEngineAbiVersion = static_cast<uint32_t>(
        settings.value(QStringLiteral("ProcessingCore/EngineAbiVersion"), 1).toUInt());
    requirements.expectedRuntimeFingerprint = settings.value(
        QStringLiteral("ProcessingCore/RuntimeFingerprint")).toString().toStdString();
    requirements.artifactSha256 = sha.toStdString();
    requirements.releaseTag = settings.value(
        QStringLiteral("ProcessingCore/ReleaseTag")).toString().toStdString();
    requirements.manifestSha256 = settings.value(
        QStringLiteral("ProcessingCore/ManifestSha256")).toString().toStdString();
    requirements.trustVerifier = trustVerifier(persistedCompatibility);
    const auto loaded = backend::processing::loadProcessingCorePlugin(
        std::filesystem::path(path.toStdString()), requirements);
    if (!loaded) {
        backend.processing().markProcessingCoreSelectionUnavailable();
        if (error) *error = QString::fromStdString(loaded.error);
        return false;
    }
    std::string activationError;
    if (!backend.processing().activateProcessingKernel(loaded.kernel, &activationError)) {
        backend.processing().markProcessingCoreSelectionUnavailable();
        if (error) *error = QString::fromStdString(activationError);
        return false;
    }
    return true;
}

void ProcessingCoreDialog::reload() {
    if (busy_) return;
    setBusy(true, tr("Loading processing-core history…"));
    const QString channel = channelBox_->currentData().toString();
    const QUrl url(QStringLiteral("%1/%2/processing-core/index.json")
                       .arg(registryBaseUrl().remove(QRegularExpression(QStringLiteral("/+$"))),
                            channel));
    if (!isHttps(url)) {
        catalog_ = {};
        versions_->clear();
        setBusy(false, tr("Processing-core registry URL must use HTTPS."));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::MaximumDownloadBufferSizeAttribute,
                         kMaxRegistryBytes);
    auto* reply = network_->get(request);
    installAbsoluteDeadline(reply, 20000);
    installDownloadLimit(reply, kMaxRegistryBytes);
    connect(reply, &QNetworkReply::finished, this, [this, reply, channel]() {
        const bool oversized = reply->bytesAvailable() > kMaxRegistryBytes;
        const QByteArray body = oversized ? QByteArray{} : reply->readAll();
        const QString networkError = reply->errorString();
        const bool requestOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!requestOk || oversized) {
            catalog_ = {};
            versions_->clear();
            setBusy(false, oversized ? tr("Registry response exceeded the size limit.")
                                     : tr("Registry unavailable: %1").arg(networkError));
            return;
        }
        catalog_ = processingcorecatalog::parseIndex(body);
        if (!catalog_.ok) {
            versions_->clear();
            setBusy(false, tr("Invalid registry index: %1").arg(catalog_.error));
            return;
        }
        if (catalog_.channel != channel) {
            versions_->clear();
            catalog_ = {};
            setBusy(false, tr("Registry returned a different channel; history refused."));
            return;
        }
        loadCanonicalActive(channel);
    });
}

void ProcessingCoreDialog::loadCanonicalActive(const QString& channel) {
    const QUrl url(QStringLiteral("%1/%2/processing-core/latest.json")
                       .arg(registryBaseUrl().remove(QRegularExpression(QStringLiteral("/+$"))),
                            channel));
    if (!isHttps(url)) {
        catalog_ = {};
        versions_->clear();
        setBusy(false, tr("Processing-core active pointer URL must use HTTPS."));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::MaximumDownloadBufferSizeAttribute,
                         kMaxRegistryBytes);
    auto* reply = network_->get(request);
    installAbsoluteDeadline(reply, 20000);
    installDownloadLimit(reply, kMaxRegistryBytes);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const bool oversized = reply->bytesAvailable() > kMaxRegistryBytes;
        const QByteArray body = oversized ? QByteArray{} : reply->readAll();
        const QString networkError = reply->errorString();
        const bool requestOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!requestOk || oversized) {
            catalog_ = {};
            versions_->clear();
            setBusy(false, oversized ? tr("Active pointer exceeded the size limit.")
                                     : tr("Active pointer unavailable: %1").arg(networkError));
            return;
        }
        const auto latest = processingcorecatalog::parseVersionManifest(body);
        if (!latest.ok) {
            catalog_ = {};
            versions_->clear();
            setBusy(false, tr("Invalid active pointer: %1").arg(latest.error));
            return;
        }
        const auto active = processingcorecatalog::validateCanonicalActive(catalog_, latest);
        if (!active.ok) {
            catalog_ = {};
            versions_->clear();
            setBusy(false, tr("Invalid active pointer: %1").arg(active.error));
            return;
        }
        catalog_.activeVersion = active.version;
        populate();
        const QString summary = tr("%n published version(s).", "", catalog_.versions.size());
        setBusy(false, active.warning.isEmpty()
                           ? summary
                           : tr("%1 %2").arg(summary, active.warning));
    });
}

void ProcessingCoreDialog::updateActiveCoreLabel() {
    const auto current = backend_.processing().activeProcessingCoreIdentity();
    activeLabel_->setText(
        backend_.processing().isProcessingCorePinSatisfied()
            ? tr("Active core: %1 · contract %2 · ABI %3 · %4")
                  .arg(QString::fromStdString(current.version))
                  .arg(current.contractVersion)
                  .arg(current.engineAbiVersion)
                  .arg(QString::fromStdString(current.source))
            : tr("Processing core unavailable: the selected version must be repaired."));
}

void ProcessingCoreDialog::populate() {
    versions_->clear();
    updateActiveCoreLabel();
    const auto current = backend_.processing().activeProcessingCoreIdentity();
    const QString hardPin =
        qEnvironmentVariable("MIB_STUDIO_PROCESSING_CORE_VERSION").trimmed();
    for (const auto& entry : catalog_.versions) {
        QString label = entry.version;
        if (entry.version == catalog_.activeVersion) label += tr("  — channel active");
        if (entry.version.toStdString() == current.version) label += tr("  — selected");
        const auto compatibility = compatibilityFor(entry);
        if (!compatibility.compatible()) label += QStringLiteral("  — ") + compatibility.diagnostic;
        auto* item = new QListWidgetItem(label, versions_);
        item->setToolTip(compatibility.diagnostic);
        item->setData(Qt::UserRole, static_cast<int>(compatibility.reason));
        if (!compatibility.compatible()) {
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        }
    }
    if (!hardPin.isEmpty()) {
        statusLabel_->setText(tr("Administrator pin: %1. Other versions cannot be activated.")
                                  .arg(hardPin));
    }
    updateButtons();
}

int ProcessingCoreDialog::selectedVersionIndex() const {
    const int row = versions_->currentRow();
    return row >= 0 && row < catalog_.versions.size() ? row : -1;
}

void ProcessingCoreDialog::prepareAndActivateSelected() {
    const int row = selectedVersionIndex();
    if (row < 0 || busy_) return;
    const auto version = catalog_.versions[row];
    const auto* selectedPlugin = processingcorecatalog::findNativePlugin(
        version, platformOs(), platformArch());
    if (!selectedPlugin) return;
    if (backend_.capture().isRunning() || backend_.isFrameRecording()) {
        setBusy(false, tr("Stop capture and frame recording before changing processing core."));
        return;
    }
    const auto plugin = *selectedPlugin;
    const auto compatibility = compatibilityFor(version);
    if (!compatibility.compatible()) {
        setBusy(false, compatibility.diagnostic);
        return;
    }
    if (version.version.toStdString() != backend_.processing().activeProcessingCoreIdentity().version) {
        const QString currentVersion = QString::fromStdString(
            backend_.processing().activeProcessingCoreIdentity().version);
        const bool downgrade = processingcorecatalog::isVersionDowngrade(
            version.version, currentVersion);
        const auto answer = QMessageBox::question(
            this,
            downgrade ? tr("Confirm processing-core downgrade")
                      : tr("Activate processing core"),
            downgrade
                ? tr("Downgrade the active processing core from %1 to %2?\n\n"
                     "Existing files keep their recorded core identity and will show a "
                     "mismatch warning; the application never switches them automatically.")
                      .arg(currentVersion, version.version)
                : tr("Prepare and activate processing core %1?\n\n"
                     "Capture, experiments, recording, and batch processing must be stopped first.")
                      .arg(version.version),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    if (version.manifestUrl.isEmpty()) {
        setBusy(false, tr("Selected version has no immutable manifest URL."));
        return;
    }
    setBusy(true, tr("Verifying immutable manifest for %1…").arg(version.version));
    QNetworkRequest request{QUrl(version.manifestUrl)};
    if (!isHttps(request.url())) {
        setBusy(false, tr("Immutable manifest URL must use HTTPS."));
        return;
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::MaximumDownloadBufferSizeAttribute,
                         kMaxRegistryBytes);
    auto* reply = network_->get(request);
    installAbsoluteDeadline(reply, 20000);
    installDownloadLimit(reply, kMaxRegistryBytes);
    connect(reply, &QNetworkReply::finished, this, [this, reply, version, plugin]() {
        const bool oversized = reply->bytesAvailable() > kMaxRegistryBytes;
        const QByteArray bytes = oversized ? QByteArray{} : reply->readAll();
        const QString networkError = reply->errorString();
        const bool requestOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!requestOk || oversized) {
            setBusy(false, oversized
                               ? tr("Immutable manifest exceeded the size limit.")
                               : tr("Immutable manifest download failed: %1").arg(networkError));
            return;
        }
        const auto manifest = processingcorecatalog::parseVersionManifest(bytes);
        const auto* manifestPlugin = manifest.ok
            ? processingcorecatalog::findNativePlugin(manifest.version, platformOs(), platformArch())
            : nullptr;
        const bool matchesIndex = manifest.ok && manifestPlugin &&
            manifest.version.channel == version.channel &&
            manifest.version.version == version.version &&
            manifest.version.contractVersion == version.contractVersion &&
            manifest.version.releaseTag == version.releaseTag &&
            manifestPlugin->filename == plugin.filename && manifestPlugin->url == plugin.url &&
            manifestPlugin->sha256 == plugin.sha256 &&
            manifestPlugin->sizeBytes == plugin.sizeBytes &&
            manifestPlugin->engineAbiVersion == plugin.engineAbiVersion &&
            manifestPlugin->contractVersion == plugin.contractVersion &&
            manifestPlugin->runtimeFingerprint == plugin.runtimeFingerprint &&
            manifestPlugin->appMinVersion == plugin.appMinVersion &&
            manifestPlugin->appMaxVersion == plugin.appMaxVersion &&
            manifestPlugin->signingScheme == plugin.signingScheme &&
            manifestPlugin->signingPublicKeySpkiBase64 == plugin.signingPublicKeySpkiBase64 &&
            manifestPlugin->signingPublicKeySpkiSha256 == plugin.signingPublicKeySpkiSha256 &&
            manifestPlugin->signingSignatureBase64 == plugin.signingSignatureBase64 &&
            manifestPlugin->signingRequired == plugin.signingRequired;
        if (!matchesIndex) {
            setBusy(false, manifest.ok
                               ? tr("Mutable index and immutable manifest disagree; activation refused.")
                               : tr("Invalid immutable manifest: %1").arg(manifest.error));
            return;
        }
        downloadAndActivate(manifest.version, *manifestPlugin, manifest.rawSha256Hex);
    });
}

void ProcessingCoreDialog::downloadAndActivate(
    const processingcorecatalog::VersionEntry& version,
    const processingcorecatalog::NativePluginEntry& plugin,
    const QByteArray& manifestSha256Hex) {
    setBusy(true, tr("Downloading %1…").arg(plugin.filename));
    const QUrl pluginUrl(plugin.url);
    if (!isHttps(pluginUrl) || plugin.sizeBytes <= 0) {
        setBusy(false, tr("Native artifact URL/size is invalid."));
        return;
    }
    if (!QDir().mkpath(cacheRoot())) {
        setBusy(false, tr("Could not create the processing-core cache directory."));
        return;
    }
    auto state = std::make_shared<NativeDownloadState>();
    state->file = std::make_shared<QTemporaryFile>(
        QDir(cacheRoot()).absoluteFilePath(QStringLiteral("download-XXXXXX")));
    if (!state->file->open()) {
        setBusy(false, tr("Could not create a staged core download."));
        return;
    }
    QNetworkRequest request{pluginUrl};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(120000);
    request.setAttribute(QNetworkRequest::MaximumDownloadBufferSizeAttribute,
                         kMaxNetworkBufferBytes);
    auto* reply = network_->get(request);
    installAbsoluteDeadline(reply, 120000);
    installDownloadLimit(reply, plugin.sizeBytes);
    connect(reply, &QNetworkReply::readyRead, this, [reply, state, plugin]() {
        if (!drainNativeDownload(reply, state, plugin.sizeBytes)) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, state, version, plugin, manifestSha256Hex]() {
        const bool drained = drainNativeDownload(reply, state, plugin.sizeBytes);
        const QString networkError = reply->errorString();
        const bool requestOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!drained || !requestOk || state->written != plugin.sizeBytes) {
            const QString detail = !state->error.isEmpty()
                ? state->error
                : (!requestOk ? tr("Download failed: %1").arg(networkError)
                              : tr("Download size does not match registry metadata."));
            setBusy(false, detail);
            return;
        }
        if (!state->file->flush()) {
            setBusy(false, tr("Could not stage the downloaded core."));
            return;
        }
        state->file->close();
        backend::processing::ProcessingCoreCacheRequest cacheRequest;
        cacheRequest.sourcePath = state->file->fileName().toStdString();
        cacheRequest.cacheRoot = std::filesystem::path(cacheRoot().toStdString());
        cacheRequest.version = version.version.toStdString();
        cacheRequest.sha256 = plugin.sha256.toStdString();
        cacheRequest.filename = plugin.filename.toStdString();
        const auto cached = backend::processing::prepareProcessingCoreArtifact(cacheRequest);
        if (!cached) {
            setBusy(false, tr("Core verification/cache failed: %1")
                               .arg(QString::fromStdString(cached.error)));
            return;
        }

        const auto loaded = backend::processing::loadProcessingCorePlugin(
            cached.pluginPath, loadRequirements(version, plugin, manifestSha256Hex));
        if (!loaded) {
            setBusy(false, tr("Core trust/load failed: %1")
                               .arg(QString::fromStdString(loaded.error)));
            return;
        }
        if (backend_.capture().isRunning() || backend_.isFrameRecording()) {
            setBusy(false, tr("Activation blocked because capture or frame recording started."));
            return;
        }
        std::string activationError;
        const bool activated = backend_.processing().activateProcessingKernel(
            loaded.kernel, &activationError, [&loaded, &cached, &plugin](std::string& commitError) {
                QSettings settings;
                QString persistenceError;
                const auto& identity = loaded.kernel->identity();
                processingcoresettings::Selection selection;
                selection.version = QString::fromStdString(identity.version);
                selection.sha256 = QString::fromStdString(identity.artifactSha256);
                selection.contractVersion = identity.contractVersion;
                selection.engineAbiVersion = identity.engineAbiVersion;
                selection.runtimeFingerprint = QString::fromStdString(identity.runtimeFingerprint);
                selection.releaseTag = QString::fromStdString(identity.releaseTag);
                selection.manifestSha256 = QString::fromStdString(identity.manifestSha256);
                selection.path = QString::fromStdString(cached.pluginPath.string());
                selection.appMinVersion = plugin.appMinVersion;
                selection.appMaxVersion = plugin.appMaxVersion;
                selection.signingScheme = plugin.signingScheme;
                selection.signingRequired = plugin.signingRequired;
                selection.signingPublicKeySpkiBase64 = plugin.signingPublicKeySpkiBase64;
                selection.signingPublicKeySpkiSha256 = plugin.signingPublicKeySpkiSha256;
                selection.signingSignatureBase64 = plugin.signingSignatureBase64;
                if (processingcoresettings::persistSelection(settings, selection,
                                                             &persistenceError)) {
                    return true;
                }
                commitError = persistenceError.toStdString();
                return false;
            });
        if (!activated) {
            SPDLOG_WARN("ProcessingCoreDialog: activation of core {} refused; previous core "
                        "remains active: {}",
                        loaded.kernel->identity().version, activationError);
            setBusy(false, tr("Activation refused; the previous core remains active: %1")
                               .arg(QString::fromStdString(activationError)));
            return;
        }
        SPDLOG_INFO("ProcessingCoreDialog: activated core {} from {}",
                    loaded.kernel->identity().version, cached.pluginPath.string());
        populate();
        setBusy(false, tr("Processing core %1 is active.").arg(version.version));
    });
}

void ProcessingCoreDialog::updateButtons() {
    prepareButton_->setEnabled(!busy_ && selectedVersionIndex() >= 0);
    refreshButton_->setEnabled(!busy_);
    channelBox_->setEnabled(!busy_);
}

void ProcessingCoreDialog::setBusy(bool busy, const QString& message) {
    busy_ = busy;
    if (!message.isEmpty()) statusLabel_->setText(message);
    updateButtons();
}

} // namespace frontend
