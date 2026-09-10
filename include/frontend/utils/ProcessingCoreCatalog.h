#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace frontend::processingcorecatalog {

enum class CompatibilityReason {
    Compatible, WrongPlatform, WrongArchitecture, UnsupportedAbi,
    UnsupportedContract, AppVersionTooOld, AppVersionTooNew,
    RuntimeConstraint, InvalidVersion, AdministratorPin
};

// Metadata eligibility only. Artifact integrity and signature checks still run
// during activation; Compatible does not mean the artifact has been trusted.
struct CompatibilityResult {
    CompatibilityReason reason{CompatibilityReason::Compatible};
    QString diagnostic;
    QString required;
    QString actual;
    bool compatible() const { return reason == CompatibilityReason::Compatible; }
};

struct CompatibilityHost {
    QString os;
    QString arch;
    QString appVersion;
    int engineAbiVersion;
    int contractVersion;
    QString runtimeFingerprint;
    QString pinnedVersion;
};

struct NativePluginEntry {
    QString filename;
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

CompatibilityResult evaluateCompatibility(const VersionEntry& version,
                                          const CompatibilityHost& host);

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
