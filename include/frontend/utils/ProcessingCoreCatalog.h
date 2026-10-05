#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace frontend::processingcorecatalog {

// ADR 0007 core lines. Each line has its own registry directory, contract,
// engine ABI and entry point; a catalog or core of one line is never
// accepted as another.
struct CoreLine {
    QString name;          // "subtract-ring" | "absdiff-laplacian"
    QString registryDir;   // "<channel>/<registryDir>/{index,latest}.json"
    int contractVersion{0};
    int engineAbiVersion{0};
    QString entrypoint;
};

const QVector<CoreLine>& coreLines();
// nullptr for an unknown line name.
const CoreLine* findCoreLine(const QString& name);

struct NativePluginEntry {
    QString filename;
    // Core line algorithm ("absdiff-laplacian"); empty on subtract-ring
    // entries, which predate core lines.
    QString algorithm;
    QString os;
    QString arch;
    QString url;
    QString sha256;
    QString runtimeFingerprint;
    QString entrypoint;
    QString appMinVersion;
    QString appMaxVersion;
    QString signingScheme;
    // Ed25519 detached-signature transport (scheme "ed25519" only): the
    // signer's base64 DER SubjectPublicKeyInfo, its SHA-256, and the base64
    // raw signature over the artifact bytes. Trust comes from the compiled
    // SPKI pin, never from these manifest fields alone.
    QString signingPublicKeySpkiBase64;
    QString signingPublicKeySpkiSha256;
    QString signingSignatureBase64;
    qint64 sizeBytes{-1};
    int engineAbiVersion{0};
    int contractVersion{0};
    bool signingRequired{false};
};

struct VersionEntry {
    QString channel;
    QString line{QStringLiteral("subtract-ring")};
    QString version;
    QString publishedAt;
    QString releaseTag;
    QString releaseUrl;
    QString manifestUrl;
    int contractVersion{0};
    QVector<NativePluginEntry> nativePlugins;
};

struct ParseResult {
    bool ok{false};
    QString error;
    QString channel;
    QString line{QStringLiteral("subtract-ring")};
    QString indexActiveVersion;
    QString activeVersion;
    QVector<VersionEntry> versions;
};

struct ManifestResult {
    bool ok{false};
    QString error;
    VersionEntry version;
    QByteArray rawSha256Hex;
};

struct ActivePointerResult {
    bool ok{false};
    QString error;
    QString warning;
    QString version;
};

ParseResult parseIndex(const QByteArray& bytes);
ManifestResult parseVersionManifest(const QByteArray& bytes);
ActivePointerResult validateCanonicalActive(const ParseResult& index,
                                            const ManifestResult& latest);
const NativePluginEntry* findNativePlugin(const VersionEntry& version,
                                          const QString& os,
                                          const QString& arch);
bool isAppCompatible(const NativePluginEntry& plugin, const QString& appVersion);
bool isProcessingContractCompatible(int requiredContractVersion,
                                    int activeContractVersion);
bool isVersionDowngrade(const QString& candidate, const QString& current);

} // namespace frontend::processingcorecatalog
