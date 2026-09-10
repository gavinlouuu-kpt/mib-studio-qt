#include "frontend/utils/ProcessingCoreCatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QVersionNumber>

#include <algorithm>
#include <cmath>

namespace frontend::processingcorecatalog {
namespace {

bool safeVersion(const QString& value) {
    static const QRegularExpression expression(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+!-]*$"));
    return value != QStringLiteral(".") && value != QStringLiteral("..") &&
           expression.match(value).hasMatch();
}

bool sha256(const QString& value) {
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}

bool httpsUrl(const QString& value) {
    const QUrl url(value);
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           !url.host().isEmpty();
}

bool nativeFilenameMatchesOs(const QString& filename, const QString& os) {
    if (os == QStringLiteral("windows")) {
        return filename.endsWith(QStringLiteral(".dll"), Qt::CaseInsensitive);
    }
    if (os == QStringLiteral("linux")) {
        return filename.endsWith(QStringLiteral(".so"), Qt::CaseInsensitive);
    }
    if (os == QStringLiteral("macos")) {
        return filename.endsWith(QStringLiteral(".dylib"), Qt::CaseInsensitive);
    }
    return false;
}

bool parseNativePlugin(const QJsonObject& object,
                       int defaultContractVersion,
                       NativePluginEntry& plugin) {
    plugin.filename = object.value(QStringLiteral("filename")).toString().trimmed();
    plugin.os = object.value(QStringLiteral("os")).toString().trimmed().toLower();
    plugin.arch = object.value(QStringLiteral("arch")).toString().trimmed().toLower();
    if (plugin.arch == QStringLiteral("amd64") || plugin.arch == QStringLiteral("x64")) {
        plugin.arch = QStringLiteral("x86_64");
    } else if (plugin.arch == QStringLiteral("arm64")) {
        plugin.arch = QStringLiteral("aarch64");
    }
    plugin.url = object.value(QStringLiteral("url")).toString().trimmed();
    plugin.sha256 = object.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    plugin.runtimeFingerprint =
        object.value(QStringLiteral("runtime_fingerprint")).toString().trimmed();
    plugin.entrypoint = object.value(QStringLiteral("entrypoint")).toString().trimmed();
    plugin.appMinVersion = object.value(QStringLiteral("app_min_version")).toString().trimmed();
    plugin.appMaxVersion = object.value(QStringLiteral("app_max_version")).toString().trimmed();
    const auto signing = object.value(QStringLiteral("signing")).toObject();
    plugin.signingScheme = signing.value(QStringLiteral("scheme")).toString().trimmed().toLower();
    if (plugin.signingScheme.isEmpty()) {
        plugin.signingScheme = signing.value(QStringLiteral("format")).toString().trimmed().toLower();
    }
    plugin.signingRequired = signing.value(QStringLiteral("required")).toBool(false);
    plugin.signingPublicKeySpkiBase64 =
        signing.value(QStringLiteral("public_key_spki_base64")).toString().trimmed();
    plugin.signingPublicKeySpkiSha256 =
        signing.value(QStringLiteral("public_key_spki_sha256")).toString().trimmed().toLower();
    plugin.signingSignatureBase64 =
        signing.value(QStringLiteral("signature_base64")).toString().trimmed();
    const auto sizeValue = object.value(QStringLiteral("size_bytes"));
    const double rawSize = sizeValue.toDouble(-1);
    const bool validSize = sizeValue.isDouble() && std::isfinite(rawSize) && rawSize > 0 &&
                           rawSize <= 512.0 * 1024.0 * 1024.0 && std::floor(rawSize) == rawSize;
    plugin.sizeBytes = validSize ? static_cast<qint64>(rawSize) : -1;
    plugin.engineAbiVersion = object.value(QStringLiteral("engine_abi_version")).toInt();
    plugin.contractVersion =
        object.value(QStringLiteral("contract_version")).toInt(defaultContractVersion);
    const auto minimum = QVersionNumber::fromString(plugin.appMinVersion);
    const auto maximum = QVersionNumber::fromString(plugin.appMaxVersion);
    const bool validRange = !minimum.isNull() &&
        (plugin.appMaxVersion.isEmpty() || (!maximum.isNull() && maximum >= minimum));
    // A detached-signature scheme must ship canonical transport material: a
    // 44-byte DER Ed25519 SubjectPublicKeyInfo, its 64-hex SHA-256, and a raw
    // 64-byte signature. Other schemes (e.g. embedded Authenticode) carry no
    // detached fields.
    bool validDetachedSignature = true;
    if (plugin.signingScheme == QStringLiteral("ed25519")) {
        const QByteArray spki = QByteArray::fromBase64(
            plugin.signingPublicKeySpkiBase64.toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        const QByteArray signatureBytes = QByteArray::fromBase64(
            plugin.signingSignatureBase64.toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        validDetachedSignature = spki.size() == 44 && signatureBytes.size() == 64 &&
                                 sha256(plugin.signingPublicKeySpkiSha256);
    }
    return !plugin.filename.isEmpty() && !plugin.filename.contains('/') &&
           !plugin.filename.contains('\\') && QFileInfo(plugin.filename).fileName() == plugin.filename &&
           nativeFilenameMatchesOs(plugin.filename, plugin.os) && !plugin.arch.isEmpty() &&
           httpsUrl(plugin.url) &&
           sha256(plugin.sha256) && plugin.sizeBytes >= 0 && plugin.engineAbiVersion > 0 &&
           plugin.contractVersion > 0 &&
           plugin.entrypoint == QStringLiteral("mib_processing_get_api") &&
           !plugin.runtimeFingerprint.isEmpty() && !plugin.signingScheme.isEmpty() &&
           plugin.signingRequired && validRange && validDetachedSignature;
}

bool parseNativePlugins(const QJsonValue& value,
                        int defaultContractVersion,
                        QVector<NativePluginEntry>& plugins,
                        QString& error) {
    if (!value.isArray()) {
        error = QStringLiteral("processing-core native_plugins must be an array");
        return false;
    }
    QSet<QString> seenPlatforms;
    for (const auto& pluginValue : value.toArray()) {
        if (!pluginValue.isObject()) {
            error = QStringLiteral("processing-core native plugin entry must be an object");
            return false;
        }
        NativePluginEntry plugin;
        if (!parseNativePlugin(pluginValue.toObject(), defaultContractVersion, plugin)) {
            error = QStringLiteral("processing-core contains invalid native metadata");
            return false;
        }
        const QString platform = plugin.os + QLatin1Char('\n') + plugin.arch;
        if (seenPlatforms.contains(platform)) {
            error = QStringLiteral("processing-core contains duplicate native platforms");
            return false;
        }
        seenPlatforms.insert(platform);
        plugins.push_back(std::move(plugin));
    }
    return true;
}

bool samePlugin(const NativePluginEntry& left, const NativePluginEntry& right) {
    return left.filename == right.filename && left.os == right.os && left.arch == right.arch &&
           left.url == right.url && left.sha256 == right.sha256 &&
           left.runtimeFingerprint == right.runtimeFingerprint &&
           left.entrypoint == right.entrypoint && left.appMinVersion == right.appMinVersion &&
           left.appMaxVersion == right.appMaxVersion &&
           left.signingScheme == right.signingScheme &&
           left.signingPublicKeySpkiBase64 == right.signingPublicKeySpkiBase64 &&
           left.signingPublicKeySpkiSha256 == right.signingPublicKeySpkiSha256 &&
           left.signingSignatureBase64 == right.signingSignatureBase64 &&
           left.signingRequired == right.signingRequired && left.sizeBytes == right.sizeBytes &&
           left.engineAbiVersion == right.engineAbiVersion &&
           left.contractVersion == right.contractVersion;
}

bool samePublishedVersion(const VersionEntry& index, const VersionEntry& manifest) {
    if (index.channel != manifest.channel || index.version != manifest.version ||
        index.publishedAt != manifest.publishedAt || index.releaseTag != manifest.releaseTag ||
        index.releaseUrl != manifest.releaseUrl || index.contractVersion != manifest.contractVersion ||
        index.nativePlugins.size() != manifest.nativePlugins.size()) {
        return false;
    }
    for (const auto& indexPlugin : index.nativePlugins) {
        const auto* manifestPlugin = findNativePlugin(manifest, indexPlugin.os, indexPlugin.arch);
        if (!manifestPlugin || !samePlugin(indexPlugin, *manifestPlugin)) return false;
    }
    return true;
}

} // namespace

ParseResult parseIndex(const QByteArray& bytes) {
    ParseResult result;
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (document.isNull() || !document.isObject()) {
        result.error = document.isNull() ? parseError.errorString()
                                         : QStringLiteral("processing-core index is not an object");
        return result;
    }
    const auto root = document.object();
    if (root.value(QStringLiteral("processing_core_index_schema_version")).toInt() != 1) {
        result.error = QStringLiteral("unsupported processing-core index schema");
        return result;
    }
    result.channel = root.value(QStringLiteral("channel")).toString().trimmed();
    result.indexActiveVersion = root.value(QStringLiteral("active_version")).toString().trimmed();
    if (result.channel.isEmpty() || result.indexActiveVersion.isEmpty()) {
        result.error = QStringLiteral("processing-core index is missing channel/active_version");
        return result;
    }

    QSet<QString> seenVersions;
    const auto versionsValue = root.value(QStringLiteral("versions"));
    if (!versionsValue.isArray()) {
        result.error = QStringLiteral("processing-core index versions must be an array");
        return result;
    }
    for (const auto& value : versionsValue.toArray()) {
        if (!value.isObject()) {
            result.error = QStringLiteral("processing-core index version entry must be an object");
            return result;
        }
        const auto object = value.toObject();
        VersionEntry entry;
        entry.channel = result.channel;
        entry.version = object.value(QStringLiteral("version")).toString().trimmed();
        entry.publishedAt = object.value(QStringLiteral("published_at")).toString().trimmed();
        entry.releaseTag = object.value(QStringLiteral("release_tag")).toString().trimmed();
        entry.releaseUrl = object.value(QStringLiteral("release_url")).toString().trimmed();
        entry.manifestUrl = object.value(QStringLiteral("manifest_url")).toString().trimmed();
        entry.contractVersion = object.value(QStringLiteral("contract_version")).toInt();
        if (!safeVersion(entry.version) || entry.contractVersion <= 0) {
            result.error = QStringLiteral("processing-core index contains an unsafe/invalid version");
            return result;
        }
        if (seenVersions.contains(entry.version)) {
            result.error = QStringLiteral("processing-core index contains duplicate versions");
            return result;
        }
        seenVersions.insert(entry.version);
        if (!httpsUrl(entry.manifestUrl)) {
            result.error = QStringLiteral("processing-core manifest URL must use HTTPS");
            return result;
        }
        if (!parseNativePlugins(object.value(QStringLiteral("native_plugins")),
                                entry.contractVersion, entry.nativePlugins, result.error))
            return result;
        result.versions.push_back(std::move(entry));
    }
    if (!safeVersion(result.indexActiveVersion)) {
        result.error = QStringLiteral("processing-core index contains an unsafe active version");
        return result;
    }
    result.ok = true;
    return result;
}

ActivePointerResult validateCanonicalActive(const ParseResult& index,
                                            const ManifestResult& latest) {
    ActivePointerResult result;
    if (!index.ok || !latest.ok) {
        result.error = QStringLiteral("processing-core index/latest input is invalid");
        return result;
    }
    if (latest.version.channel != index.channel) {
        result.error = QStringLiteral("processing-core latest pointer returned a different channel");
        return result;
    }
    const auto entry = std::find_if(index.versions.cbegin(), index.versions.cend(),
                                    [&latest](const VersionEntry& candidate) {
                                        return candidate.version == latest.version.version;
                                    });
    if (entry == index.versions.cend()) {
        result.error = QStringLiteral("processing-core latest version is absent from history");
        return result;
    }
    if (!samePublishedVersion(*entry, latest.version)) {
        result.error = QStringLiteral("processing-core latest pointer disagrees with history");
        return result;
    }
    result.version = latest.version.version;
    if (index.indexActiveVersion != result.version) {
        result.warning = QStringLiteral(
            "Registry publication is incomplete: latest.json remains active; "
            "index.active_version was ignored");
    }
    result.ok = true;
    return result;
}

ManifestResult parseVersionManifest(const QByteArray& bytes) {
    ManifestResult result;
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (document.isNull() || !document.isObject()) {
        result.error = document.isNull() ? parseError.errorString()
                                         : QStringLiteral("processing-core manifest is not an object");
        return result;
    }
    const auto root = document.object();
    if (root.value(QStringLiteral("processing_core_manifest_schema_version")).toInt() != 2) {
        result.error = QStringLiteral("unsupported processing-core manifest schema");
        return result;
    }
    VersionEntry entry;
    entry.channel = root.value(QStringLiteral("channel")).toString().trimmed();
    entry.version = root.value(QStringLiteral("version")).toString().trimmed();
    entry.contractVersion = root.value(QStringLiteral("contract_version")).toInt();
    entry.publishedAt = root.value(QStringLiteral("published_at")).toString().trimmed();
    const auto wheel = root.value(QStringLiteral("wheel")).toObject();
    entry.releaseTag = wheel.value(QStringLiteral("release_tag")).toString().trimmed();
    entry.releaseUrl = wheel.value(QStringLiteral("release_url")).toString().trimmed();
    if (entry.channel.isEmpty() || !safeVersion(entry.version) || entry.contractVersion <= 0 ||
        wheel.value(QStringLiteral("version")).toString().trimmed() != entry.version) {
        result.error = QStringLiteral("processing-core manifest identity is inconsistent");
        return result;
    }
    if (!parseNativePlugins(root.value(QStringLiteral("native_plugins")),
                            entry.contractVersion, entry.nativePlugins, result.error))
        return result;
    for (const auto& plugin : entry.nativePlugins) {
        if (plugin.contractVersion != entry.contractVersion) {
            result.error = QStringLiteral("processing-core native contract does not match manifest");
            return result;
        }
    }
    result.version = std::move(entry);
    result.rawSha256Hex = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    result.ok = true;
    return result;
}

const NativePluginEntry* findNativePlugin(const VersionEntry& version,
                                          const QString& os,
                                          const QString& arch) {
    const QString normalizedOs = os.trimmed().toLower();
    const QString normalizedArch = arch.trimmed().toLower();
    for (const auto& plugin : version.nativePlugins) {
        if (plugin.os == normalizedOs && plugin.arch == normalizedArch) return &plugin;
    }
    return nullptr;
}

CompatibilityResult evaluateCompatibility(const VersionEntry& version,
                                          const CompatibilityHost& host) {
    using R = CompatibilityReason;
    const auto reject = [](R reason, const QString& description,
                           const QString& required, const QString& actual) {
        return CompatibilityResult{reason,
            QStringLiteral("%1 (required: %2; current: %3)").arg(description, required, actual),
            required, actual};
    };
    const auto* plugin = findNativePlugin(version, host.os, host.arch);
    if (!plugin) {
        const bool hasOs = std::any_of(version.nativePlugins.begin(), version.nativePlugins.end(),
            [&](const NativePluginEntry& entry) {
                return entry.os.compare(host.os.trimmed(), Qt::CaseInsensitive) == 0;
            });
        return reject(hasOs ? R::WrongArchitecture : R::WrongPlatform,
                      hasOs ? QStringLiteral("No artifact for this architecture")
                            : QStringLiteral("No artifact for this operating system"),
                      QStringLiteral("matching native artifact"), hasOs ? host.arch : host.os);
    }
    if (plugin->engineAbiVersion != host.engineAbiVersion)
        return reject(R::UnsupportedAbi, QStringLiteral("Unsupported engine ABI"),
                      QString::number(plugin->engineAbiVersion), QString::number(host.engineAbiVersion));
    if (plugin->contractVersion != host.contractVersion)
        return reject(R::UnsupportedContract, QStringLiteral("Unsupported processing contract"),
                      QString::number(plugin->contractVersion), QString::number(host.contractVersion));
    const auto current = QVersionNumber::fromString(host.appVersion);
    const auto minimum = QVersionNumber::fromString(plugin->appMinVersion);
    const auto maximum = QVersionNumber::fromString(plugin->appMaxVersion);
    if (current.isNull() || minimum.isNull() ||
        (!plugin->appMaxVersion.isEmpty() && maximum.isNull()))
        return reject(R::InvalidVersion, QStringLiteral("Cannot evaluate application version bounds"),
                      plugin->appMinVersion + QStringLiteral("..") + plugin->appMaxVersion, host.appVersion);
    if (current < minimum)
        return reject(R::AppVersionTooOld, QStringLiteral("Application is too old"),
                      plugin->appMinVersion, host.appVersion);
    if (!plugin->appMaxVersion.isEmpty() && current > maximum)
        return reject(R::AppVersionTooNew, QStringLiteral("Application is too new"),
                      plugin->appMaxVersion, host.appVersion);
    if (plugin->runtimeFingerprint != host.runtimeFingerprint)
        return reject(R::RuntimeConstraint, QStringLiteral("Runtime fingerprint mismatch"),
                      plugin->runtimeFingerprint, host.runtimeFingerprint);
    if (!host.pinnedVersion.isEmpty() && host.pinnedVersion != version.version)
        return reject(R::AdministratorPin, QStringLiteral("Administrator pin prevents activation"),
                      host.pinnedVersion, version.version);
    return {};
}

bool isAppCompatible(const NativePluginEntry& plugin, const QString& appVersion) {
    const auto current = QVersionNumber::fromString(appVersion);
    const auto minimum = QVersionNumber::fromString(plugin.appMinVersion);
    const auto maximum = QVersionNumber::fromString(plugin.appMaxVersion);
    return !current.isNull() && !minimum.isNull() && current >= minimum &&
           (plugin.appMaxVersion.isEmpty() || (!maximum.isNull() && current <= maximum));
}

bool isProcessingContractCompatible(int requiredContractVersion,
                                    int activeContractVersion) {
    return requiredContractVersion <= 0 || activeContractVersion <= 0 ||
           requiredContractVersion == activeContractVersion;
}

bool isVersionDowngrade(const QString& candidate, const QString& current) {
    if (candidate == current) return false;
    qsizetype candidateSuffix = 0;
    qsizetype currentSuffix = 0;
    const auto candidateCore = QVersionNumber::fromString(candidate, &candidateSuffix);
    const auto currentCore = QVersionNumber::fromString(current, &currentSuffix);
    if (candidateCore.isNull() || currentCore.isNull()) return false;
    if (candidateCore != currentCore) return candidateCore < currentCore;

    const bool candidatePrerelease = candidateSuffix < candidate.size();
    const bool currentPrerelease = currentSuffix < current.size();
    if (candidatePrerelease != currentPrerelease) return candidatePrerelease;
    return candidatePrerelease && candidate.mid(candidateSuffix) < current.mid(currentSuffix);
}

} // namespace frontend::processingcorecatalog
