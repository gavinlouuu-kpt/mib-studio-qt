#include "frontend/system/ProfileManager.h"
#include "frontend/utils/ProcessingCoreCatalog.h"

#include <algorithm>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>

#include <spdlog/spdlog.h>

#include "frontend/utils/ConfigPathManager.h"

namespace {

constexpr int kNetworkTimeoutMs = 8000;

QString currentUtcString() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString titleCaseFromProfileName(QString name) {
    name.replace('_', ' ');
    name.replace('-', ' ');
    return name.trimmed().split(' ', Qt::SkipEmptyParts).join(' ');
}

QJsonValue parseValueFromString(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed == QStringLiteral("true")) return QJsonValue(true);
    if (trimmed == QStringLiteral("false")) return QJsonValue(false);
    if (trimmed == QStringLiteral("null") || trimmed == QStringLiteral("undefined")) return QJsonValue();

    bool ok = false;
    const double asDouble = trimmed.toDouble(&ok);
    if (ok && !trimmed.isEmpty()) {
        return QJsonValue(asDouble);
    }

    if (!trimmed.isEmpty() && (trimmed.startsWith('{') || trimmed.startsWith('['))) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError) {
            if (doc.isObject()) return doc.object();
            if (doc.isArray()) return doc.array();
        }
    }

    return QJsonValue(trimmed);
}

QString valueTypeName(const QJsonValue& value) {
    if (value.isNull()) return QStringLiteral("null");
    if (value.isUndefined()) return QStringLiteral("undefined");
    if (value.isBool()) return QStringLiteral("bool");
    if (value.isDouble()) return QStringLiteral("number");
    if (value.isString()) return QStringLiteral("string");
    if (value.isArray()) return QStringLiteral("array");
    if (value.isObject()) return QStringLiteral("object");
    return QStringLiteral("unknown");
}

QString scalarToString(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 15);
    if (value.isNull()) return QStringLiteral("null");
    if (value.isUndefined()) return QStringLiteral("undefined");
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return QString();
}

void mergeMissingDefaults(QJsonObject& target, const QJsonObject& defaults) {
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        const QString key = it.key();
        const QJsonValue defaultValue = it.value();
        if (!target.contains(key)) {
            target.insert(key, defaultValue);
            continue;
        }
        QJsonValue targetValue = target.value(key);
        if (targetValue.isObject() && defaultValue.isObject()) {
            QJsonObject child = targetValue.toObject();
            mergeMissingDefaults(child, defaultValue.toObject());
            target.insert(key, child);
        }
    }
}

QString normalizeProfileLabel(const QString& name, const QString& displayName, bool updateAvailable, bool dirty, bool remoteManaged, bool incompatible) {
    QStringList tags;
    if (remoteManaged) tags << QStringLiteral("remote");
    if (updateAvailable) tags << QStringLiteral("update available");
    if (dirty) tags << QStringLiteral("dirty");
    if (incompatible) tags << QStringLiteral("incompatible");
    const QString tagText = tags.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(tags.join(QStringLiteral(", ")));
    const QString base = displayName.trimmed().isEmpty() ? titleCaseFromProfileName(name) : displayName.trimmed();
    return base + tagText;
}

bool isHighRiskPath(const QString& path) {
    static const QStringList exactHighRisk = {
        QStringLiteral("pixel_to_micron_factor"),
        QStringLiteral("image_processing.target_group.enabled"),
        QStringLiteral("image_processing.target_group.area_min"),
        QStringLiteral("image_processing.target_group.area_max"),
        QStringLiteral("image_processing.target_group.deformability_min"),
        QStringLiteral("image_processing.target_group.deformability_max"),
        QStringLiteral("image_processing.target_group.emodulus_enabled"),
        QStringLiteral("image_processing.target_group.emodulus_min"),
        QStringLiteral("image_processing.target_group.emodulus_max"),
        QStringLiteral("image_processing.multi_image.enabled"),
        QStringLiteral("image_processing.multi_image.count")
    };
    if (exactHighRisk.contains(path)) return true;
    return path.startsWith(QStringLiteral("image_processing.area_threshold_")) ||
           path.startsWith(QStringLiteral("image_processing.deformability_threshold_")) ||
           path.startsWith(QStringLiteral("image_processing.ring_ratio_")) ||
           path.startsWith(QStringLiteral("image_processing.filters.")) ||
           path.startsWith(QStringLiteral("image_processing.target_group.")) ||
           path.startsWith(QStringLiteral("image_processing.multi_image.")) ||
           path == QStringLiteral("pixel_to_micron_factor");
}

bool isMediumRiskPath(const QString& path) {
    return path == QStringLiteral("buffer_threshold") ||
           path.startsWith(QStringLiteral("realtime_processing.")) ||
           path == QStringLiteral("camera.frame_delivery_mode") ||
           path == QStringLiteral("target_fps");
}

QString configSourceForPath(const QString& path) {
    if (path.startsWith(QStringLiteral("profile_meta_schema_version")) ||
        path.startsWith(QStringLiteral("source.")) ||
        path.startsWith(QStringLiteral("revision")) ||
        path.startsWith(QStringLiteral("config_schema_version")) ||
        path.startsWith(QStringLiteral("config_sha256")) ||
        path.startsWith(QStringLiteral("camera_script_sha256")) ||
        path.startsWith(QStringLiteral("last_checked_utc")) ||
        path.startsWith(QStringLiteral("last_updated_utc"))) {
        return QStringLiteral("Metadata");
    }
    if (path.startsWith(QStringLiteral("camera_script"))) {
        return QStringLiteral("Camera script");
    }
    if (path.startsWith(QStringLiteral("camera."))) {
        // Acquisition settings (e.g. camera.frame_delivery_mode) live in
        // config.json; they are not camera-script content.
        return QStringLiteral("Config");
    }
    return QStringLiteral("Config");
}

void copyFileIfPresent(const QString& source, const QString& destination, QString* errorOut) {
    if (!QFile::exists(source)) {
        return;
    }
    QFileInfo dstInfo(destination);
    QDir().mkpath(dstInfo.absolutePath());
    if (QFile::exists(destination)) {
        QFile::remove(destination);
    }
    if (!QFile::copy(source, destination) && errorOut && errorOut->isEmpty()) {
        *errorOut = QStringLiteral("Failed to copy %1 to %2").arg(source, destination);
    }
}

QJsonObject readObjectOrEmpty(const QJsonDocument& doc) {
    return doc.isObject() ? doc.object() : QJsonObject();
}

} // namespace

namespace frontend {

QString ProfileManager::profilesBaseDir() const {
    return QDir(ConfigPathManager::getUserConfigDirectory()).absoluteFilePath(QStringLiteral("profiles"));
}

QString ProfileManager::profileDirPath(const QString& profileName) const {
    return QDir(profilesBaseDir()).absoluteFilePath(profileName);
}

QString ProfileManager::profileJsonPath(const QString& profileName) const {
    return QDir(profileDirPath(profileName)).absoluteFilePath(QStringLiteral("config.json"));
}

QString ProfileManager::profileJsPath(const QString& profileName) const {
    return QDir(profileDirPath(profileName)).absoluteFilePath(QStringLiteral("egrabberConfig.js"));
}

QString ProfileManager::profileMetaPath(const QString& profileName) const {
    return QDir(profileDirPath(profileName)).absoluteFilePath(QStringLiteral("profile.meta.json"));
}

QString ProfileManager::catalogUrlForChannel(const QString& channel) const {
    const QString trimmedChannel = channel.trimmed().isEmpty() ? QStringLiteral("stable") : channel.trimmed();
    return QStringLiteral("https://updates.yofo.bio/profiles/%1/catalog.json").arg(trimmedChannel);
}

QUrl ProfileManager::catalogUrlFromEnvOrDefault(const QString& channel) const {
    const QString override = qEnvironmentVariable("MIB_STUDIO_PROFILE_CATALOG_URL");
    if (!override.trimmed().isEmpty()) {
        const QUrl url(override.trimmed());
        if (url.isValid() && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"))) {
            return url;
        }
        SPDLOG_WARN("ProfileManager: ignoring invalid MIB_STUDIO_PROFILE_CATALOG_URL='{}'", override.toStdString());
    }
    return QUrl(catalogUrlForChannel(channel));
}

std::optional<QByteArray> ProfileManager::readFileBytes(const QString& path, QString* errorOut) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1: %2").arg(path, file.errorString());
        }
        return std::nullopt;
    }
    return file.readAll();
}

std::optional<QJsonDocument> ProfileManager::loadJsonDocument(const QString& path, QString* errorOut) const {
    const auto bytes = readFileBytes(path, errorOut);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(*bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid JSON in %1: %2").arg(path, parseError.errorString());
        }
        return std::nullopt;
    }
    return doc;
}

bool ProfileManager::saveJsonDocumentAtomic(const QString& path, const QJsonDocument& doc, QString* errorOut) const {
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to open %1 for write: %2").arg(path, out.errorString());
        }
        return false;
    }
    const QByteArray payload = doc.toJson(QJsonDocument::Indented);
    if (out.write(payload) != payload.size()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to write %1: %2").arg(path, out.errorString());
        }
        return false;
    }
    if (!out.commit()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Failed to commit %1: %2").arg(path, out.errorString());
        }
        return false;
    }
    return true;
}

QByteArray ProfileManager::sha256Hex(const QByteArray& bytes) const {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool ProfileManager::downloadUrlBlocking(const QUrl& url, QByteArray* outBytes, QString* errorOut) const {
    if (!url.isValid()) {
        if (errorOut) *errorOut = QStringLiteral("Invalid URL: %1").arg(url.toString());
        return false;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kNetworkTimeoutMs);

    QNetworkReply* reply = manager.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(kNetworkTimeoutMs);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start();
    loop.exec();
    timeout.stop();

    const QByteArray body = reply->readAll();
    const auto error = reply->error();
    const QString errorString = reply->errorString();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (error != QNetworkReply::NoError) {
        if (errorOut) {
            QString detail = errorString;
            if (httpStatus > 0) {
                detail += QStringLiteral(" (HTTP %1)").arg(httpStatus);
            }
            if (!body.isEmpty()) {
                detail += QStringLiteral("\n%1").arg(QString::fromUtf8(body.left(500)));
            }
            *errorOut = detail;
        }
        return false;
    }

    if (outBytes) {
        *outBytes = body;
    }
    return true;
}

std::optional<ProfileManager::Catalog> ProfileManager::fetchCatalog(const QUrl& url, QString* errorOut) const {
    QByteArray body;
    if (!downloadUrlBlocking(url, &body, errorOut)) {
        return std::nullopt;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Invalid catalog JSON: %1").arg(parseError.errorString());
        }
        return std::nullopt;
    }

    const QJsonObject obj = doc.object();
    Catalog catalog;
    catalog.catalogSchemaVersion = obj.value(QStringLiteral("catalog_schema_version")).toInt(0);
    catalog.channel = obj.value(QStringLiteral("channel")).toString();
    const QString publishedAt = obj.value(QStringLiteral("published_at")).toString();
    if (!publishedAt.trimmed().isEmpty()) {
        catalog.publishedAt = QDateTime::fromString(publishedAt, Qt::ISODateWithMs);
        if (!catalog.publishedAt.isValid()) {
            catalog.publishedAt = QDateTime::fromString(publishedAt, Qt::ISODate);
        }
    }

    const QJsonArray profiles = obj.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue& profileValue : profiles) {
        if (!profileValue.isObject()) {
            continue;
        }
        const QJsonObject profileObj = profileValue.toObject();
        CatalogEntry entry;
        entry.profileId = profileObj.value(QStringLiteral("profile_id")).toString().trimmed();
        entry.displayName = profileObj.value(QStringLiteral("display_name")).toString();
        entry.description = profileObj.value(QStringLiteral("description")).toString();
        entry.revision = profileObj.value(QStringLiteral("revision")).toString();
        entry.profileMetaUrl = QUrl(profileObj.value(QStringLiteral("profile_meta_url")).toString());
        entry.configUrl = QUrl(profileObj.value(QStringLiteral("config_url")).toString());
        entry.cameraScriptUrl = QUrl(profileObj.value(QStringLiteral("camera_script_url")).toString());
        entry.configSha256 = profileObj.value(QStringLiteral("config_sha256")).toString().trimmed().toLower();
        entry.cameraScriptSha256 = profileObj.value(QStringLiteral("camera_script_sha256")).toString().trimmed().toLower();
        entry.appMinVersion = profileObj.value(QStringLiteral("app_min_version")).toString();
        entry.appMaxVersion = profileObj.value(QStringLiteral("app_max_version")).toString();
        entry.processingContractVersion =
            profileObj.value(QStringLiteral("processing_contract_version")).toInt(0);

        if (entry.profileId.isEmpty() || !entry.configUrl.isValid()) {
            continue;
        }
        catalog.profiles.append(entry);
    }

    if (catalog.catalogSchemaVersion <= 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Catalog is missing catalog_schema_version.");
        }
        return std::nullopt;
    }

    return catalog;
}

std::optional<ProfileManager::CatalogEntry> ProfileManager::findCatalogEntry(const Catalog& catalog, const QString& profileId) const {
    for (const CatalogEntry& entry : catalog.profiles) {
        if (entry.profileId == profileId) {
            return entry;
        }
    }
    return std::nullopt;
}

ProfileManager::Metadata ProfileManager::metadataForLocalProfile(const QString& profileId,
                                                                 const QString& displayName,
                                                                 const QString& description,
                                                                 const QString& configSha256,
                                                                 const QString& scriptSha256,
                                                                 const QString& /*configPath*/,
                                                                 const QString& /*scriptPath*/) {
    Metadata metadata;
    metadata.profileId = profileId;
    metadata.displayName = displayName;
    metadata.description = description;
    metadata.sourceType = QStringLiteral("local-generated");
    metadata.channel = QStringLiteral("local");
    metadata.catalogUrl.clear();
    metadata.revision = currentUtcString();
    metadata.configSchemaVersion = 0;
    metadata.configSha256 = configSha256;
    metadata.cameraScriptSha256 = scriptSha256;
    metadata.appMinVersion = QCoreApplication::applicationVersion();
    metadata.appMaxVersion.clear();
    metadata.lastCheckedUtc = QDateTime();
    metadata.lastUpdatedUtc = QDateTime::currentDateTimeUtc();
    return metadata;
}

QJsonObject ProfileManager::metadataToJson(const Metadata& metadata) {
    QJsonObject obj;
    obj.insert(QStringLiteral("profile_meta_schema_version"), metadata.profileMetaSchemaVersion);
    obj.insert(QStringLiteral("profile_id"), metadata.profileId);
    obj.insert(QStringLiteral("display_name"), metadata.displayName);
    obj.insert(QStringLiteral("description"), metadata.description);
    QJsonObject source;
    source.insert(QStringLiteral("type"), metadata.sourceType);
    source.insert(QStringLiteral("channel"), metadata.channel);
    source.insert(QStringLiteral("catalog_url"), metadata.catalogUrl);
    obj.insert(QStringLiteral("source"), source);
    obj.insert(QStringLiteral("revision"), metadata.revision);
    obj.insert(QStringLiteral("config_schema_version"), metadata.configSchemaVersion);
    obj.insert(QStringLiteral("config_sha256"), metadata.configSha256);
    obj.insert(QStringLiteral("camera_script_sha256"), metadata.cameraScriptSha256);
    obj.insert(QStringLiteral("app_min_version"), metadata.appMinVersion);
    if (!metadata.appMaxVersion.trimmed().isEmpty()) {
        obj.insert(QStringLiteral("app_max_version"), metadata.appMaxVersion);
    } else {
        obj.insert(QStringLiteral("app_max_version"), QJsonValue::Null);
    }
    if (metadata.processingContractVersion > 0) {
        obj.insert(QStringLiteral("processing_contract_version"),
                   metadata.processingContractVersion);
    } else {
        obj.insert(QStringLiteral("processing_contract_version"), QJsonValue::Null);
    }
    if (metadata.lastCheckedUtc.isValid()) {
        obj.insert(QStringLiteral("last_checked_utc"), metadata.lastCheckedUtc.toString(Qt::ISODateWithMs));
    } else {
        obj.insert(QStringLiteral("last_checked_utc"), QJsonValue::Null);
    }
    if (metadata.lastUpdatedUtc.isValid()) {
        obj.insert(QStringLiteral("last_updated_utc"), metadata.lastUpdatedUtc.toString(Qt::ISODateWithMs));
    } else {
        obj.insert(QStringLiteral("last_updated_utc"), QJsonValue::Null);
    }
    return obj;
}

std::optional<ProfileManager::Metadata> ProfileManager::metadataFromJson(const QJsonObject& obj, QString* errorOut) {
    Q_UNUSED(errorOut);
    Metadata metadata;
    metadata.profileMetaSchemaVersion = obj.value(QStringLiteral("profile_meta_schema_version")).toInt(0);
    metadata.profileId = obj.value(QStringLiteral("profile_id")).toString();
    metadata.displayName = obj.value(QStringLiteral("display_name")).toString();
    metadata.description = obj.value(QStringLiteral("description")).toString();
    const QJsonObject source = obj.value(QStringLiteral("source")).toObject();
    metadata.sourceType = source.value(QStringLiteral("type")).toString();
    metadata.channel = source.value(QStringLiteral("channel")).toString();
    metadata.catalogUrl = source.value(QStringLiteral("catalog_url")).toString();
    metadata.revision = obj.value(QStringLiteral("revision")).toString();
    metadata.configSchemaVersion = obj.value(QStringLiteral("config_schema_version")).toInt(0);
    metadata.configSha256 = obj.value(QStringLiteral("config_sha256")).toString().trimmed().toLower();
    metadata.cameraScriptSha256 = obj.value(QStringLiteral("camera_script_sha256")).toString().trimmed().toLower();
    metadata.appMinVersion = obj.value(QStringLiteral("app_min_version")).toString();
    const QString maxVersion = obj.value(QStringLiteral("app_max_version")).toString();
    metadata.appMaxVersion = maxVersion;
    metadata.processingContractVersion =
        obj.value(QStringLiteral("processing_contract_version")).toInt(0);
    const QString lastChecked = obj.value(QStringLiteral("last_checked_utc")).toString();
    if (!lastChecked.trimmed().isEmpty()) {
        metadata.lastCheckedUtc = QDateTime::fromString(lastChecked, Qt::ISODateWithMs);
        if (!metadata.lastCheckedUtc.isValid()) {
            metadata.lastCheckedUtc = QDateTime::fromString(lastChecked, Qt::ISODate);
        }
    }
    const QString lastUpdated = obj.value(QStringLiteral("last_updated_utc")).toString();
    if (!lastUpdated.trimmed().isEmpty()) {
        metadata.lastUpdatedUtc = QDateTime::fromString(lastUpdated, Qt::ISODateWithMs);
        if (!metadata.lastUpdatedUtc.isValid()) {
            metadata.lastUpdatedUtc = QDateTime::fromString(lastUpdated, Qt::ISODate);
        }
    }

    if (metadata.profileMetaSchemaVersion <= 0) {
        return std::nullopt;
    }
    return metadata;
}

std::optional<ProfileManager::Metadata> ProfileManager::readMetadataFile(const QString& path, QString* errorOut) const {
    const auto doc = loadJsonDocument(path, errorOut);
    if (!doc.has_value() || !doc->isObject()) {
        return std::nullopt;
    }
    auto metadata = metadataFromJson(doc->object(), errorOut);
    return metadata;
}

bool ProfileManager::writeMetadataFile(const QString& path, const Metadata& metadata, QString* errorOut) const {
    return saveJsonDocumentAtomic(path, QJsonDocument(metadataToJson(metadata)), errorOut);
}

bool ProfileManager::writeProfileMetadata(const LocalProfile& profile, const Catalog* catalog, QString* errorOut) const {
    Metadata metadata = profile.metadata;
    if (catalog && profile.remoteEntry.has_value()) {
        metadata.sourceType = QStringLiteral("r2-public-catalog");
        metadata.channel = catalog->channel;
        metadata.catalogUrl = catalogUrlForChannel(catalog->channel);
        metadata.lastCheckedUtc = QDateTime::currentDateTimeUtc();
        metadata.lastUpdatedUtc = QDateTime::currentDateTimeUtc();
        metadata.profileMetaSchemaVersion = 1;
        metadata.profileId = profile.profileName;
        if (metadata.displayName.trimmed().isEmpty()) {
            metadata.displayName = profile.displayName;
        }
        if (metadata.description.trimmed().isEmpty()) {
            metadata.description = profile.description;
        }
        metadata.configSha256 = profile.metadata.configSha256;
        metadata.cameraScriptSha256 = profile.metadata.cameraScriptSha256;
        if (profile.remoteEntry->appMinVersion.trimmed().isEmpty()) {
            metadata.appMinVersion = QCoreApplication::applicationVersion();
        } else {
            metadata.appMinVersion = profile.remoteEntry->appMinVersion;
        }
        metadata.appMaxVersion = profile.remoteEntry->appMaxVersion;
        metadata.processingContractVersion =
            profile.remoteEntry->processingContractVersion;
        metadata.revision = profile.remoteEntry->revision;
    }
    return writeMetadataFile(profile.metaPath, metadata, errorOut);
}

QVector<ProfileManager::LocalProfile> ProfileManager::scanLocalProfiles(
    bool ensureMetadata,
    const Catalog* catalog,
    QString* errorOut,
    int activeProcessingContractVersion) const {
    QVector<LocalProfile> out;
    QDir baseDir(profilesBaseDir());
    if (!baseDir.exists()) {
        if (!baseDir.mkpath(QStringLiteral("."))) {
            if (errorOut) {
                *errorOut = QStringLiteral("Failed to create profiles directory: %1").arg(baseDir.absolutePath());
            }
            return out;
        }
    }

    const QFileInfoList entries = baseDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& entry : entries) {
        LocalProfile profile;
        profile.profileName = entry.fileName();
        profile.profileDir = entry.absoluteFilePath();
        profile.configPath = profileJsonPath(profile.profileName);
        profile.scriptPath = profileJsPath(profile.profileName);
        profile.metaPath = profileMetaPath(profile.profileName);
        profile.hasConfig = QFile::exists(profile.configPath);
        profile.hasScript = QFile::exists(profile.scriptPath);
        if (!profile.hasConfig) {
            continue;
        }

        QString localError;
        const auto configDoc = loadJsonDocument(profile.configPath, &localError);
        if (!configDoc.has_value() || !configDoc->isObject()) {
            SPDLOG_WARN("ProfileManager: skipping profile '{}' due to invalid config.json: {}", profile.profileName.toStdString(), localError.toStdString());
            continue;
        }

        const QJsonObject configObj = configDoc->object();
        const QString displayName = configObj.value(QStringLiteral("display_name")).toString();
        profile.displayName = displayName.isEmpty() ? titleCaseFromProfileName(profile.profileName) : displayName;
        profile.description = configObj.value(QStringLiteral("description")).toString();

        const auto configBytes = readFileBytes(profile.configPath, &localError);
        const QByteArray configHash = configBytes.has_value() ? sha256Hex(*configBytes) : QByteArray();
        QByteArray scriptHash;
        if (profile.hasScript) {
            const auto scriptBytes = readFileBytes(profile.scriptPath, &localError);
            if (scriptBytes.has_value()) {
                scriptHash = sha256Hex(*scriptBytes);
            }
        }

        auto existingMeta = readMetadataFile(profile.metaPath, &localError);
        if (!existingMeta.has_value() && ensureMetadata) {
            profile.metadata = metadataForLocalProfile(profile.profileName,
                                                       profile.displayName,
                                                       profile.description,
                                                       QString::fromLatin1(configHash),
                                                       QString::fromLatin1(scriptHash),
                                                       profile.configPath,
                                                       profile.scriptPath);
            if (!writeMetadataFile(profile.metaPath, profile.metadata, &localError)) {
                SPDLOG_WARN("ProfileManager: failed to write metadata for '{}': {}", profile.profileName.toStdString(), localError.toStdString());
            } else {
                profile.hasMetadata = true;
            }
        } else if (existingMeta.has_value()) {
            profile.metadata = *existingMeta;
            profile.hasMetadata = true;
            const bool localManaged = profile.metadata.sourceType != QStringLiteral("r2-public-catalog");
            const bool hashMismatch = (!profile.metadata.configSha256.trimmed().isEmpty() &&
                                       !configHash.isEmpty() &&
                                       profile.metadata.configSha256.compare(QString::fromLatin1(configHash), Qt::CaseInsensitive) != 0) ||
                                      (!scriptHash.isEmpty() &&
                                       !profile.metadata.cameraScriptSha256.trimmed().isEmpty() &&
                                       profile.metadata.cameraScriptSha256.compare(QString::fromLatin1(scriptHash), Qt::CaseInsensitive) != 0);
            if (ensureMetadata && localManaged && hashMismatch) {
                profile.metadata.configSha256 = QString::fromLatin1(configHash);
                profile.metadata.cameraScriptSha256 = QString::fromLatin1(scriptHash);
                profile.metadata.configSchemaVersion = configObj.value(QStringLiteral("config_schema_version")).toInt(1);
                profile.metadata.displayName = profile.displayName;
                profile.metadata.description = profile.description;
                profile.metadata.lastUpdatedUtc = QDateTime::currentDateTimeUtc();
                if (!writeMetadataFile(profile.metaPath, profile.metadata, &localError)) {
                    SPDLOG_WARN("ProfileManager: failed to refresh metadata for '{}': {}", profile.profileName.toStdString(), localError.toStdString());
                }
            }
        } else {
            profile.metadata = metadataForLocalProfile(profile.profileName,
                                                       profile.displayName,
                                                       profile.description,
                                                       QString::fromLatin1(configHash),
                                                       QString::fromLatin1(scriptHash),
                                                       profile.configPath,
                                                       profile.scriptPath);
        }

        if (!profile.metadata.displayName.trimmed().isEmpty()) {
            profile.displayName = profile.metadata.displayName;
        }

        profile.localOnly = !(profile.metadata.sourceType == QStringLiteral("r2-public-catalog"));
        profile.incompatible = false;
        profile.dirty = !profile.metadata.configSha256.trimmed().isEmpty() &&
                        !configHash.isEmpty() &&
                        profile.metadata.configSha256.compare(QString::fromLatin1(configHash), Qt::CaseInsensitive) != 0;

        if (catalog) {
            const auto remote = findCatalogEntry(*catalog, profile.profileName);
            if (remote.has_value()) {
                profile.remoteEntry = remote;
                profile.remoteEntry->displayName = remote->displayName.isEmpty() ? profile.displayName : remote->displayName;
                profile.remoteEntry->description = remote->description;
                profile.remoteEntry->profileId = remote->profileId;
                profile.remoteEntry->revision = remote->revision;
                profile.updateAvailable = !profile.metadata.configSha256.trimmed().isEmpty() &&
                                          !remote->configSha256.trimmed().isEmpty() &&
                                          profile.metadata.configSha256.compare(remote->configSha256, Qt::CaseInsensitive) != 0;
                profile.localOnly = false;
                const QVersionNumber currentVersion = QVersionNumber::fromString(QCoreApplication::applicationVersion());
                const QVersionNumber minVersion = QVersionNumber::fromString(remote->appMinVersion);
                const QVersionNumber maxVersion = QVersionNumber::fromString(remote->appMaxVersion);
                bool compatible = true;
                if (!minVersion.isNull() && !currentVersion.isNull() && QVersionNumber::compare(currentVersion, minVersion) < 0) {
                    compatible = false;
                }
                if (!maxVersion.isNull() && !currentVersion.isNull() && QVersionNumber::compare(currentVersion, maxVersion) > 0) {
                    compatible = false;
                }
                profile.incompatible = !compatible;
            }
        }
        const int requiredProcessingContract = profile.remoteEntry.has_value()
            ? profile.remoteEntry->processingContractVersion
            : profile.metadata.processingContractVersion;
        if (!processingcorecatalog::isProcessingContractCompatible(
                requiredProcessingContract,
                activeProcessingContractVersion)) {
            profile.incompatible = true;
        }

        out.append(profile);
    }

    std::sort(out.begin(), out.end(), [](const LocalProfile& a, const LocalProfile& b) {
        return a.displayName.toLower() < b.displayName.toLower();
    });

    return out;
}

std::optional<QJsonObject> ProfileManager::normalizeConfigForSchema(const QJsonDocument& input, QString* errorOut) const {
    if (!input.isObject()) {
        if (errorOut) *errorOut = QStringLiteral("Config root must be a JSON object.");
        return std::nullopt;
    }

    QJsonObject root = input.object();
    const auto defaultsDoc = loadJsonDocument(QStringLiteral(":/defaults/config.json"), errorOut);
    if (defaultsDoc.has_value() && defaultsDoc->isObject()) {
        mergeMissingDefaults(root, defaultsDoc->object());
    }

    const int schemaVersion = root.value(QStringLiteral("config_schema_version")).toInt(0);
    if (schemaVersion <= 0) {
        root.insert(QStringLiteral("config_schema_version"), 1);
    }
    return root;
}

void ProfileManager::flattenForDiff(const QJsonValue& value, const QString& path, QMap<QString, FlattenedValue>& out) {
    if (value.isObject()) {
        if (!path.isEmpty()) {
            out.insert(path, FlattenedValue{QStringLiteral("object"), value});
        }
        const QJsonObject obj = value.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QString childPath = path.isEmpty() ? it.key() : path + QStringLiteral(".") + it.key();
            flattenForDiff(it.value(), childPath, out);
        }
        return;
    }
    if (value.isArray()) {
        if (!path.isEmpty()) {
            out.insert(path, FlattenedValue{QStringLiteral("array"), value});
        }
        const QJsonArray arr = value.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            const QString childPath = QStringLiteral("%1[%2]").arg(path).arg(i);
            flattenForDiff(arr.at(i), childPath, out);
        }
        return;
    }
    out.insert(path, FlattenedValue{valueTypeName(value), value});
}

QString ProfileManager::jsonValueToString(const QJsonValue& value) {
    return scalarToString(value);
}

QString ProfileManager::jsonTypeName(const QJsonValue& value) {
    return valueTypeName(value);
}

QString ProfileManager::diffPathForKey(const QString& parent, const QString& key) {
    return parent.isEmpty() ? key : parent + QStringLiteral(".") + key;
}

QString ProfileManager::diffStatusToString(DiffStatus status) {
    switch (status) {
    case DiffStatus::Added: return QStringLiteral("Added");
    case DiffStatus::Removed: return QStringLiteral("Removed");
    case DiffStatus::TypeChanged: return QStringLiteral("Type Changed");
    case DiffStatus::Changed:
    default:
        return QStringLiteral("Changed");
    }
}

QString ProfileManager::riskForPath(const QString& path) {
    if (isHighRiskPath(path)) {
        return QStringLiteral("High");
    }
    if (isMediumRiskPath(path)) {
        return QStringLiteral("Medium");
    }
    return QStringLiteral("Low");
}

QString ProfileManager::sourceForPath(const QString& path, const QString& defaultSource) {
    const QString source = configSourceForPath(path);
    return source.isEmpty() ? defaultSource : source;
}

QVector<ProfileManager::DiffRow> ProfileManager::diffConfigDocuments(const QJsonDocument& localDoc, const QJsonDocument& remoteDoc) const {
    QVector<DiffRow> rows;

    QString localError;
    QString remoteError;
    const auto normalizedLocal = normalizeConfigForSchema(localDoc, &localError);
    const auto normalizedRemote = normalizeConfigForSchema(remoteDoc, &remoteError);
    if (!normalizedLocal.has_value() || !normalizedRemote.has_value()) {
        return rows;
    }

    QMap<QString, FlattenedValue> localFlat;
    QMap<QString, FlattenedValue> remoteFlat;
    flattenForDiff(QJsonValue(*normalizedLocal), QString(), localFlat);
    flattenForDiff(QJsonValue(*normalizedRemote), QString(), remoteFlat);

    QSet<QString> allPaths;
    for (auto it = localFlat.constBegin(); it != localFlat.constEnd(); ++it) {
        if (!it.key().isEmpty()) {
            allPaths.insert(it.key());
        }
    }
    for (auto it = remoteFlat.constBegin(); it != remoteFlat.constEnd(); ++it) {
        if (!it.key().isEmpty()) {
            allPaths.insert(it.key());
        }
    }

    QStringList sortedPaths = QStringList(allPaths.cbegin(), allPaths.cend());
    std::sort(sortedPaths.begin(), sortedPaths.end(), [](const QString& a, const QString& b) {
        return a.toLower() < b.toLower();
    });

    for (const QString& path : sortedPaths) {
        const bool hasLocal = localFlat.contains(path);
        const bool hasRemote = remoteFlat.contains(path);
        if (!hasLocal && !hasRemote) {
            continue;
        }

        const FlattenedValue localValue = hasLocal ? localFlat.value(path) : FlattenedValue{};
        const FlattenedValue remoteValue = hasRemote ? remoteFlat.value(path) : FlattenedValue{};
        const QString localType = localValue.type;
        const QString remoteType = remoteValue.type;

        DiffStatus status = DiffStatus::Changed;
        if (!hasLocal && hasRemote) {
            status = DiffStatus::Added;
        } else if (hasLocal && !hasRemote) {
            status = DiffStatus::Removed;
        } else if (localType != remoteType) {
            status = DiffStatus::TypeChanged;
        } else if (localValue.value != remoteValue.value) {
            status = DiffStatus::Changed;
        } else {
            continue;
        }

        DiffRow row;
        row.path = path;
        row.status = status;
        row.localValue = hasLocal ? jsonValueToString(localValue.value) : QStringLiteral("—");
        row.remoteValue = hasRemote ? jsonValueToString(remoteValue.value) : QStringLiteral("—");
        row.risk = riskForPath(path);
        row.source = sourceForPath(path);
        rows.append(row);
    }

    return rows;
}

QVector<ProfileManager::DiffRow> ProfileManager::diffConfigBytes(const QByteArray& localBytes, const QByteArray& remoteBytes, QString* errorOut) const {
    QJsonParseError localParse{};
    QJsonParseError remoteParse{};
    const QJsonDocument localDoc = QJsonDocument::fromJson(localBytes, &localParse);
    const QJsonDocument remoteDoc = QJsonDocument::fromJson(remoteBytes, &remoteParse);
    if (localParse.error != QJsonParseError::NoError) {
        if (errorOut) *errorOut = QStringLiteral("Invalid local JSON: %1").arg(localParse.errorString());
        return {};
    }
    if (remoteParse.error != QJsonParseError::NoError) {
        if (errorOut) *errorOut = QStringLiteral("Invalid remote JSON: %1").arg(remoteParse.errorString());
        return {};
    }
    return diffConfigDocuments(localDoc, remoteDoc);
}

bool ProfileManager::installRemoteProfile(const CatalogEntry& remoteEntry, const QString& localProfileName, QString* errorOut) const {
    if (remoteEntry.profileId.trimmed().isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Remote profile is missing a profile_id.");
        return false;
    }
    if (localProfileName.trimmed().isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Local profile name is empty.");
        return false;
    }

    QByteArray configBytes;
    QByteArray scriptBytes;
    QByteArray metaBytes;
    if (!downloadUrlBlocking(remoteEntry.configUrl, &configBytes, errorOut)) {
        return false;
    }
    if (!remoteEntry.configSha256.trimmed().isEmpty() &&
        sha256Hex(configBytes).compare(remoteEntry.configSha256.toLatin1(), Qt::CaseInsensitive) != 0) {
        if (errorOut) {
            *errorOut = QStringLiteral("Config SHA-256 mismatch for %1.").arg(remoteEntry.profileId);
        }
        return false;
    }

    if (remoteEntry.cameraScriptUrl.isValid()) {
        if (!downloadUrlBlocking(remoteEntry.cameraScriptUrl, &scriptBytes, errorOut)) {
            return false;
        }
        if (!remoteEntry.cameraScriptSha256.trimmed().isEmpty() &&
            sha256Hex(scriptBytes).compare(remoteEntry.cameraScriptSha256.toLatin1(), Qt::CaseInsensitive) != 0) {
            if (errorOut) {
                *errorOut = QStringLiteral("Camera script SHA-256 mismatch for %1.").arg(remoteEntry.profileId);
            }
            return false;
        }
    }

    Metadata metadata;
    if (remoteEntry.profileMetaUrl.isValid()) {
        QByteArray downloadedMeta;
        if (downloadUrlBlocking(remoteEntry.profileMetaUrl, &downloadedMeta, errorOut)) {
            metaBytes = downloadedMeta;
        }
    }

    QJsonObject mergedConfig;
    {
        QJsonParseError parseError{};
        const QJsonDocument configDoc = QJsonDocument::fromJson(configBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !configDoc.isObject()) {
            if (errorOut) {
                *errorOut = QStringLiteral("Downloaded config is invalid JSON: %1").arg(parseError.errorString());
            }
            return false;
        }
        const auto normalized = normalizeConfigForSchema(configDoc, errorOut);
        if (!normalized.has_value()) {
            return false;
        }
        mergedConfig = *normalized;
        configBytes = QJsonDocument(mergedConfig).toJson(QJsonDocument::Indented);
    }

    if (!metaBytes.isEmpty()) {
        QJsonParseError parseError{};
        const QJsonDocument metaDoc = QJsonDocument::fromJson(metaBytes, &parseError);
        if (parseError.error == QJsonParseError::NoError && metaDoc.isObject()) {
            const auto parsed = metadataFromJson(metaDoc.object(), errorOut);
            if (parsed.has_value()) {
                metadata = *parsed;
            }
        }
    }

    if (metadata.profileId.trimmed().isEmpty()) {
        metadata = metadataForLocalProfile(remoteEntry.profileId,
                                           remoteEntry.displayName,
                                           remoteEntry.description,
                                           QString::fromLatin1(sha256Hex(configBytes)),
                                           QString::fromLatin1(sha256Hex(scriptBytes)),
                                           profileJsonPath(localProfileName),
                                           profileJsPath(localProfileName));
    }
    metadata.profileMetaSchemaVersion = 1;
    metadata.profileId = remoteEntry.profileId;
    metadata.displayName = remoteEntry.displayName.isEmpty() ? titleCaseFromProfileName(localProfileName) : remoteEntry.displayName;
    metadata.description = remoteEntry.description;
    metadata.sourceType = QStringLiteral("r2-public-catalog");
    metadata.channel = QStringLiteral("stable");
    metadata.catalogUrl = catalogUrlForChannel(QStringLiteral("stable"));
    metadata.revision = remoteEntry.revision;
    metadata.configSchemaVersion = mergedConfig.value(QStringLiteral("config_schema_version")).toInt(1);
    metadata.configSha256 = QString::fromLatin1(sha256Hex(configBytes));
    metadata.cameraScriptSha256 = QString::fromLatin1(sha256Hex(scriptBytes));
    metadata.appMinVersion = remoteEntry.appMinVersion;
    metadata.appMaxVersion = remoteEntry.appMaxVersion;
    metadata.lastCheckedUtc = QDateTime::currentDateTimeUtc();
    metadata.lastUpdatedUtc = QDateTime::currentDateTimeUtc();

    const QString baseDir = profilesBaseDir();
    const QString targetDir = profileDirPath(localProfileName);
    const QString backupDir = QDir(baseDir).absoluteFilePath(QStringLiteral("backups/%1-%2")
                                                                .arg(localProfileName,
                                                                     QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"))));
    QDir().mkpath(backupDir);

    if (QDir(targetDir).exists()) {
        const QFileInfoList currentFiles = QDir(targetDir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fileInfo : currentFiles) {
            copyFileIfPresent(fileInfo.absoluteFilePath(), QDir(backupDir).absoluteFilePath(fileInfo.fileName()), errorOut);
        }
        if (errorOut && !errorOut->isEmpty()) {
            return false;
        }
    }

    if (!QDir().mkpath(targetDir)) {
        if (errorOut) *errorOut = QStringLiteral("Failed to create target profile directory: %1").arg(targetDir);
        return false;
    }

    {
        QSaveFile configOut(profileJsonPath(localProfileName));
        if (!configOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errorOut) *errorOut = QStringLiteral("Failed to open config.json for write: %1").arg(configOut.errorString());
            return false;
        }
        const QByteArray payload = QJsonDocument(mergedConfig).toJson(QJsonDocument::Indented);
        if (configOut.write(payload) != payload.size() || !configOut.commit()) {
            if (errorOut) *errorOut = QStringLiteral("Failed to write config.json: %1").arg(configOut.errorString());
            return false;
        }
    }

    if (!scriptBytes.isEmpty()) {
        QSaveFile scriptOut(profileJsPath(localProfileName));
        if (!scriptOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errorOut) *errorOut = QStringLiteral("Failed to open egrabberConfig.js for write: %1").arg(scriptOut.errorString());
            return false;
        }
        if (scriptOut.write(scriptBytes) != scriptBytes.size() || !scriptOut.commit()) {
            if (errorOut) *errorOut = QStringLiteral("Failed to write egrabberConfig.js: %1").arg(scriptOut.errorString());
            return false;
        }
    }

    if (!writeMetadataFile(profileMetaPath(localProfileName), metadata, errorOut)) {
        return false;
    }

    SPDLOG_INFO("ProfileManager: installed remote profile '{}' into {}", remoteEntry.profileId.toStdString(), targetDir.toStdString());
    return true;
}

bool ProfileManager::duplicateProfileAsLocal(const QString& sourceProfileName,
                                             const QString& destinationProfileName,
                                             QString* errorOut) const {
    if (sourceProfileName.trimmed().isEmpty() || destinationProfileName.trimmed().isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("Source or destination profile name is empty.");
        return false;
    }
    if (sourceProfileName == destinationProfileName) {
        if (errorOut) *errorOut = QStringLiteral("Destination profile name must differ from source.");
        return false;
    }

    const QString sourceDir = profileDirPath(sourceProfileName);
    const QString destinationDir = profileDirPath(destinationProfileName);
    if (!QDir(sourceDir).exists()) {
        if (errorOut) *errorOut = QStringLiteral("Source profile does not exist: %1").arg(sourceProfileName);
        return false;
    }
    if (QDir(destinationDir).exists()) {
        if (errorOut) *errorOut = QStringLiteral("Destination profile already exists: %1").arg(destinationProfileName);
        return false;
    }

    if (!QDir().mkpath(destinationDir)) {
        if (errorOut) *errorOut = QStringLiteral("Failed to create destination profile directory: %1").arg(destinationDir);
        return false;
    }

    copyFileIfPresent(profileJsonPath(sourceProfileName), profileJsonPath(destinationProfileName), errorOut);
    copyFileIfPresent(profileJsPath(sourceProfileName), profileJsPath(destinationProfileName), errorOut);

    QString configErr;
    const auto configDoc = loadJsonDocument(profileJsonPath(destinationProfileName), &configErr);
    if (!configDoc.has_value()) {
        if (errorOut) *errorOut = configErr;
        return false;
    }
    const auto normalized = normalizeConfigForSchema(*configDoc, &configErr);
    if (!normalized.has_value()) {
        if (errorOut) *errorOut = configErr;
        return false;
    }

    Metadata metadata = metadataForLocalProfile(destinationProfileName,
                                                titleCaseFromProfileName(destinationProfileName),
                                                QString(),
                                                QString::fromLatin1(sha256Hex(QJsonDocument(*normalized).toJson(QJsonDocument::Indented))),
                                                QString::fromLatin1(sha256Hex(readFileBytes(profileJsPath(destinationProfileName)).value_or(QByteArray()))),
                                                profileJsonPath(destinationProfileName),
                                                profileJsPath(destinationProfileName));
    metadata.profileMetaSchemaVersion = 1;
    metadata.displayName = titleCaseFromProfileName(destinationProfileName);
    metadata.sourceType = QStringLiteral("local-generated");
    metadata.channel = QStringLiteral("local");
    metadata.catalogUrl.clear();
    metadata.configSchemaVersion = normalized->value(QStringLiteral("config_schema_version")).toInt(1);
    metadata.lastUpdatedUtc = QDateTime::currentDateTimeUtc();
    metadata.lastCheckedUtc = QDateTime();

    if (!writeMetadataFile(profileMetaPath(destinationProfileName), metadata, errorOut)) {
        return false;
    }

    SPDLOG_INFO("ProfileManager: duplicated '{}' to '{}'", sourceProfileName.toStdString(), destinationProfileName.toStdString());
    return true;
}

} // namespace frontend
