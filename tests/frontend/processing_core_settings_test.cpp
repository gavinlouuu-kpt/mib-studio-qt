#include "frontend/utils/ProcessingCoreSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <QSettings>

#include <filesystem>
#include <fstream>

namespace {

frontend::processingcoresettings::Selection selection(const QString& version, const QString& path) {
    frontend::processingcoresettings::Selection result;
    result.version = version;
    result.sha256 = version + QStringLiteral("-sha");
    result.contractVersion = 1;
    result.engineAbiVersion = 1;
    result.runtimeFingerprint = QStringLiteral("test-runtime");
    result.releaseTag = QStringLiteral("mib-processing-v") + version;
    result.manifestSha256 = version + QStringLiteral("-manifest");
    result.path = path;
    result.appMinVersion = version;
    result.appMaxVersion = version;
    result.signingScheme = QStringLiteral("authenticode");
    result.signingRequired = true;
    return result;
}

} // namespace

int main() {
    mib::test::TempDir root("processing_core_settings");
    const auto settingsParent = root / "settings";
    std::filesystem::create_directories(settingsParent);
    const auto settingsPath = settingsParent / "selection.ini";
    QSettings settings(QString::fromStdString(settingsPath.string()), QSettings::IniFormat);

    QString error;
    const auto previous = selection(QStringLiteral("1.0.0"),
                                    QString::fromStdString((root / "old-core.dll").string()));
    MIB_REQUIRE(frontend::processingcoresettings::persistSelection(settings, previous, &error),
                error.toStdString());
    MIB_EXPECT(settings.value(QStringLiteral("ProcessingCore/Version")).toString() ==
                   QStringLiteral("1.0.0"),
               "complete selection persists on a writable settings path");
    MIB_EXPECT(settings.value(QStringLiteral("ProcessingCore/SigningScheme")).toString() ==
                       QStringLiteral("authenticode") &&
                   settings.value(QStringLiteral("ProcessingCore/SigningRequired")).toBool(),
               "platform trust policy is persisted with the exact selection");

    const auto backupParent = root / "settings-backup";
    std::filesystem::rename(settingsParent, backupParent);
    {
        std::ofstream blocker(settingsParent);
        MIB_REQUIRE(blocker.good(), "replace settings parent with a regular-file blocker");
    }

    error.clear();
    const auto candidate = selection(QStringLiteral("2.0.0"),
                                     QString::fromStdString((root / "new-core.dll").string()));
    MIB_EXPECT(!frontend::processingcoresettings::persistSelection(settings, candidate, &error),
               "selection write fails cleanly when the settings parent is not a directory");
    MIB_EXPECT(!error.isEmpty(), "selection write failure includes a diagnostic");
    MIB_EXPECT(settings.value(QStringLiteral("ProcessingCore/Version")).toString() ==
                   QStringLiteral("1.0.0"),
               "failed sync restores the previous logical selection in memory");
    MIB_EXPECT(settings.value(QStringLiteral("ProcessingCore/Path"))
                   .toString()
                   .endsWith(QStringLiteral("old-core.dll")),
               "failed sync restores every previous selection field");

    QSettings persisted(QString::fromStdString((backupParent / "selection.ini").string()),
                        QSettings::IniFormat);
    MIB_EXPECT(persisted.value(QStringLiteral("ProcessingCore/Version")).toString() ==
                   QStringLiteral("1.0.0"),
               "failed candidate write leaves the last synchronized selection on disk");

    // Core line (T1.1c): persisted with the selection; older selections read as subtract-ring.
    {
        const auto linePath = root / "line.ini";
        QSettings lineSettings(QString::fromStdString(linePath.string()), QSettings::IniFormat);
        MIB_EXPECT(frontend::processingcoresettings::persistedLine(lineSettings) == QStringLiteral("subtract-ring"),
                   "a selection without a line reads as subtract-ring");
        auto c2 = selection(QStringLiteral("0.1.0"), QString::fromStdString((root / "c2.dll").string()));
        c2.line = QStringLiteral("absdiff-laplacian");
        c2.contractVersion = 2;
        c2.engineAbiVersion = 2;
        QString lineError;
        MIB_REQUIRE(frontend::processingcoresettings::persistSelection(lineSettings, c2, &lineError),
                    lineError.toStdString());
        MIB_EXPECT(frontend::processingcoresettings::persistedLine(lineSettings) ==
                       QStringLiteral("absdiff-laplacian"),
                   "the selection's line round-trips");
    }
    return mib::test::exitCode();
}
