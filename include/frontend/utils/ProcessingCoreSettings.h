#pragma once

#include <QString>

#include <cstdint>

class QSettings;

namespace frontend::processingcoresettings {

struct Selection {
    QString version;
    QString line;  // core line (ADR 0007); empty persists as "subtract-ring"
    QString sha256;
    std::uint32_t contractVersion{0};
    std::uint32_t engineAbiVersion{0};
    QString runtimeFingerprint;
    QString releaseTag;
    QString manifestSha256;
    QString path;
    QString appMinVersion;
    QString appMaxVersion;
    QString signingScheme;
    QString signingPublicKeySpkiBase64;
    QString signingPublicKeySpkiSha256;
    QString signingSignatureBase64;
    bool signingRequired{false};
};

// Writes the complete explicit selection and synchronizes it before returning
// success. On a sync failure, the prior in-memory values are restored so a
// failed candidate never becomes the logical selection for this process.
bool persistSelection(QSettings& settings, const Selection& selection, QString* error = nullptr);

// The persisted selection's core line; selections saved before lines existed
// (and the bundled core) read as "subtract-ring".
QString persistedLine(const QSettings& settings);

} // namespace frontend::processingcoresettings
