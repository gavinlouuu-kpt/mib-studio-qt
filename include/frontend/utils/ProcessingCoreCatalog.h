#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace frontend::processingcorecatalog {

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
    // Core line (ADR 0007): "subtract-ring" for the legacy registry schemas,
    // else the line manifest's `line` (e.g. "absdiff-laplacian").
    QString line;
    QString channel;
    QString version;
    QString publishedAt;
    QString releaseTag;
    QString releaseUrl;
    QString manifestUrl;
    int contractVersion{0};
    int engineAbiVersion{0};
    QVector<NativePluginEntry> nativePlugins;
};

struct ParseResult {
    bool ok{false};
    QString error;
    QString line;
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

// The app and profile a core is offered to.
struct HostIdentity {
    QString os;
    QString arch;
    QString appVersion;
    QString runtimeFingerprint;
    int profileContractVersion{0};  // 0 = unknown/unset: the contract is not enforced
};

// One row of the Processing Core dialog: a published core version on one line.
struct CoreOption {
    VersionEntry version;
    NativePluginEntry plugin;  // this platform's artifact (valid when hasPlugin)
    bool hasPlugin{false};
    bool channelActive{false};
    QString disabledReason;  // empty = offerable
};

// (1, 1, mib_processing_get_api) or (2, 2, mib_processing_get_api_v2).
bool isLoadableAbi(const NativePluginEntry& plugin);

// Merges the registry trees (in the given order, each in its index order) and
// decides, per version, whether this host may offer it and why not.
QVector<CoreOption> buildCoreOptions(const QVector<ParseResult>& trees, const HostIdentity& host);

// The core the app has loaded. artifactSha256 may be empty when unknown.
struct ActiveCore {
    QString line;
    QString version;
    QString artifactSha256;
};

enum class ActivationKind { AlreadyActive, Upgrade, Downgrade, LineSwitch };

// What activating this row would do. The artifact hash decides "already
// active" when both sides know it; otherwise line + version decide.
ActivationKind classifyActivation(const CoreOption& option, const ActiveCore& active);

// The line of the loaded core: the persisted line when the persisted selection
// is the loaded artifact (by hash, or by version when a hash is missing),
// otherwise the bundled line, subtract-ring.
QString resolveActiveLine(const QString& persistedLine, const QString& persistedVersion,
                          const QString& persistedSha256, const QString& activeVersion,
                          const QString& activeSha256);

} // namespace frontend::processingcorecatalog
