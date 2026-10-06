#pragma once

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QUrl>

#include <optional>

namespace frontend {

class ProfileManager final {
public:
    struct Metadata {
        int profileMetaSchemaVersion = 1;
        QString profileId;
        QString displayName;
        QString description;
        QString sourceType;
        QString channel;
        QString catalogUrl;
        QString revision;
        int configSchemaVersion = 0;
        QString configSha256;
        QString cameraScriptSha256;
        QString appMinVersion;
        QString appMaxVersion;
        int processingContractVersion = 0;
        QDateTime lastCheckedUtc;
        QDateTime lastUpdatedUtc;
    };

    struct CatalogEntry {
        QString profileId;
        QString displayName;
        QString description;
        QString revision;
        QUrl profileMetaUrl;
        QUrl configUrl;
        QUrl cameraScriptUrl;
        QString configSha256;
        QString cameraScriptSha256;
        QString appMinVersion;
        QString appMaxVersion;
        int processingContractVersion = 0;
    };

    struct Catalog {
        int catalogSchemaVersion = 0;
        QString channel;
        QDateTime publishedAt;
        QVector<CatalogEntry> profiles;
    };

    enum class DiffStatus {
        Added,
        Removed,
        Changed,
        TypeChanged
    };

    struct DiffRow {
        QString path;
        DiffStatus status = DiffStatus::Changed;
        QString localValue;
        QString remoteValue;
        QString risk;
        QString source;
    };

    struct LocalProfile {
        QString profileName;
        QString displayName;
        QString description;
        QString profileDir;
        QString configPath;
        QString scriptPath;
        QString metaPath;
        bool hasConfig = false;
        bool hasScript = false;
        bool hasMetadata = false;
        bool localOnly = true;
        bool dirty = false;
        bool incompatible = false;
        bool updateAvailable = false;
        Metadata metadata;
        std::optional<CatalogEntry> remoteEntry;
    };

    QString profilesBaseDir() const;
    QString profileDirPath(const QString& profileName) const;
    QString profileJsonPath(const QString& profileName) const;
    QString profileJsPath(const QString& profileName) const;
    QString profileMetaPath(const QString& profileName) const;

    QString catalogUrlForChannel(const QString& channel = QStringLiteral("stable")) const;
    QUrl catalogUrlFromEnvOrDefault(const QString& channel = QStringLiteral("stable")) const;

    QVector<LocalProfile> scanLocalProfiles(bool ensureMetadata = true,
                                            const Catalog* catalog = nullptr,
                                            QString* errorOut = nullptr,
                                            int activeProcessingContractVersion = 0) const;
    std::optional<Catalog> fetchCatalog(const QUrl& url, QString* errorOut = nullptr) const;
    std::optional<CatalogEntry> findCatalogEntry(const Catalog& catalog, const QString& profileId) const;

    // Explicit Contract-1 -> Contract-2 copy-upgrade. Returns a schema-2 config
    // document migrated from `v1Doc` via the Qt-free backend migrator
    // (backend::processing::contract::migrateProfileConfigV1ToV2): unrelated
    // values preserved, ring configuration removed, identity preprocessing
    // installed, Laplacian gate disabled. Never rewrites the source; the caller
    // writes the result to a new/destination profile. Fails closed on a
    // non-v1 or malformed document.
    std::optional<QJsonDocument> copyUpgradeConfigToV2(const QJsonDocument& v1Doc,
                                                       QString* errorOut = nullptr) const;

    std::optional<QJsonDocument> loadJsonDocument(const QString& path, QString* errorOut = nullptr) const;
    bool saveJsonDocumentAtomic(const QString& path, const QJsonDocument& doc, QString* errorOut = nullptr) const;

    QByteArray sha256Hex(const QByteArray& bytes) const;
    std::optional<QByteArray> readFileBytes(const QString& path, QString* errorOut = nullptr) const;
    bool downloadUrlBlocking(const QUrl& url, QByteArray* outBytes, QString* errorOut = nullptr) const;

    std::optional<QJsonObject> normalizeConfigForSchema(const QJsonDocument& input, QString* errorOut = nullptr) const;
    QVector<DiffRow> diffConfigDocuments(const QJsonDocument& localDoc, const QJsonDocument& remoteDoc) const;
    QVector<DiffRow> diffConfigBytes(const QByteArray& localBytes, const QByteArray& remoteBytes, QString* errorOut = nullptr) const;

    bool installRemoteProfile(const CatalogEntry& remoteEntry,
                              const QString& localProfileName,
                              QString* errorOut = nullptr) const;

    bool duplicateProfileAsLocal(const QString& sourceProfileName,
                                 const QString& destinationProfileName,
                                 QString* errorOut = nullptr) const;

    static QString diffStatusToString(DiffStatus status);
    static QString riskForPath(const QString& path);
    static QString sourceForPath(const QString& path, const QString& defaultSource = QStringLiteral("Config"));

private:
    struct FlattenedValue {
        QString type;
        QJsonValue value;
    };

    bool writeProfileMetadata(const LocalProfile& profile, const Catalog* catalog, QString* errorOut) const;
    bool writeMetadataFile(const QString& path, const Metadata& metadata, QString* errorOut) const;
    std::optional<Metadata> readMetadataFile(const QString& path, QString* errorOut = nullptr) const;
    static Metadata metadataForLocalProfile(const QString& profileId,
                                            const QString& displayName,
                                            const QString& description,
                                            const QString& configSha256,
                                            const QString& scriptSha256,
                                            const QString& configPath,
                                            const QString& scriptPath);
    static QJsonObject metadataToJson(const Metadata& metadata);
    static std::optional<Metadata> metadataFromJson(const QJsonObject& obj, QString* errorOut = nullptr);
    static void flattenForDiff(const QJsonValue& value, const QString& path, QMap<QString, FlattenedValue>& out);
    static QString jsonValueToString(const QJsonValue& value);
    static QString jsonTypeName(const QJsonValue& value);
    static QString diffPathForKey(const QString& parent, const QString& key);
};

} // namespace frontend
