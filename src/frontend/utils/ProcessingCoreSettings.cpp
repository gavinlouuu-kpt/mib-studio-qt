#include "frontend/utils/ProcessingCoreSettings.h"

#include <QSettings>
#include <QStringList>
#include <QVariant>

#include <vector>

namespace frontend::processingcoresettings {
namespace {

const QStringList& selectionKeys() {
    static const QStringList keys{
        QStringLiteral("ProcessingCore/Version"),
        QStringLiteral("ProcessingCore/Line"),
        QStringLiteral("ProcessingCore/Sha256"),
        QStringLiteral("ProcessingCore/ContractVersion"),
        QStringLiteral("ProcessingCore/EngineAbiVersion"),
        QStringLiteral("ProcessingCore/RuntimeFingerprint"),
        QStringLiteral("ProcessingCore/ReleaseTag"),
        QStringLiteral("ProcessingCore/ManifestSha256"),
        QStringLiteral("ProcessingCore/Path"),
        QStringLiteral("ProcessingCore/AppMinVersion"),
        QStringLiteral("ProcessingCore/AppMaxVersion"),
        QStringLiteral("ProcessingCore/SigningScheme"),
        QStringLiteral("ProcessingCore/SigningRequired"),
        QStringLiteral("ProcessingCore/SigningPublicKeySpkiBase64"),
        QStringLiteral("ProcessingCore/SigningPublicKeySpkiSha256"),
        QStringLiteral("ProcessingCore/SigningSignatureBase64"),
    };
    return keys;
}

struct PreviousValue {
    QString key;
    QVariant value;
    bool existed{false};
};

void restorePreviousValues(QSettings& settings, const std::vector<PreviousValue>& previous) {
    for (const auto& entry : previous) {
        if (entry.existed)
            settings.setValue(entry.key, entry.value);
        else
            settings.remove(entry.key);
    }
    settings.sync();
}

} // namespace

QString persistedLine(const QSettings& settings) {
    const QString line = settings.value(QStringLiteral("ProcessingCore/Line")).toString().trimmed();
    return line.isEmpty() ? QStringLiteral("subtract-ring") : line;
}

bool persistSelection(QSettings& settings, const Selection& selection, QString* error) {
    std::vector<PreviousValue> previous;
    previous.reserve(static_cast<std::size_t>(selectionKeys().size()));
    for (const QString& key : selectionKeys()) {
        previous.push_back({key, settings.value(key), settings.contains(key)});
    }

    settings.setValue(QStringLiteral("ProcessingCore/Version"), selection.version);
    settings.setValue(QStringLiteral("ProcessingCore/Line"),
                      selection.line.isEmpty() ? QStringLiteral("subtract-ring") : selection.line);
    settings.setValue(QStringLiteral("ProcessingCore/Sha256"), selection.sha256);
    settings.setValue(QStringLiteral("ProcessingCore/ContractVersion"),
                      static_cast<qulonglong>(selection.contractVersion));
    settings.setValue(QStringLiteral("ProcessingCore/EngineAbiVersion"),
                      static_cast<qulonglong>(selection.engineAbiVersion));
    settings.setValue(QStringLiteral("ProcessingCore/RuntimeFingerprint"),
                      selection.runtimeFingerprint);
    settings.setValue(QStringLiteral("ProcessingCore/ReleaseTag"), selection.releaseTag);
    settings.setValue(QStringLiteral("ProcessingCore/ManifestSha256"), selection.manifestSha256);
    settings.setValue(QStringLiteral("ProcessingCore/Path"), selection.path);
    settings.setValue(QStringLiteral("ProcessingCore/AppMinVersion"), selection.appMinVersion);
    settings.setValue(QStringLiteral("ProcessingCore/AppMaxVersion"), selection.appMaxVersion);
    settings.setValue(QStringLiteral("ProcessingCore/SigningScheme"), selection.signingScheme);
    settings.setValue(QStringLiteral("ProcessingCore/SigningRequired"), selection.signingRequired);
    settings.setValue(QStringLiteral("ProcessingCore/SigningPublicKeySpkiBase64"),
                      selection.signingPublicKeySpkiBase64);
    settings.setValue(QStringLiteral("ProcessingCore/SigningPublicKeySpkiSha256"),
                      selection.signingPublicKeySpkiSha256);
    settings.setValue(QStringLiteral("ProcessingCore/SigningSignatureBase64"),
                      selection.signingSignatureBase64);
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;

    const auto failureStatus = settings.status();
    restorePreviousValues(settings, previous);
    if (error) {
        *error = QStringLiteral("Could not persist processing-core selection to %1 (status %2).")
                     .arg(settings.fileName())
                     .arg(static_cast<int>(failureStatus));
    }
    return false;
}

} // namespace frontend::processingcoresettings
