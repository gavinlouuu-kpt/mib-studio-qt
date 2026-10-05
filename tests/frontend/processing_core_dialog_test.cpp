#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "frontend/dialogs/ProcessingCoreDialog.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>

namespace {
// The Windows release runner killed this test at the CTest timeout without
// any captured output; unbuffered stage markers make the next stall
// attributable from CI logs alone.
void stage(const char* name) {
    std::fprintf(stderr, "stage: %s\n", name);
    std::fflush(stderr);
}
}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("all"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL",
            QByteArrayLiteral("http://invalid-registry.example"));
    // Keep the LUT catalog offline: AppBackend::initialize resolves the
    // manifest through a synchronous QEventLoop fetch, and the Windows
    // release runner's proxy resolution can stall past the CTest timeout.
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL",
            QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));

#ifdef Q_OS_WIN
    // The Conan Qt 6.7.3 offscreen platform deadlocks inside the
    // QApplication constructor on Windows release runners (runs 29331984211
    // and 29333657030 stalled here for the full CTest timeout with no
    // output). The native windows platform boots headlessly; the dialog is
    // constructed but never shown, so nothing becomes visible.
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows"));
#endif
    stage("creating QApplication");
    QApplication app(argc, argv);
    // The core-line selector persists to QSettings; keep it out of the
    // developer's real settings.
    QStandardPaths::setTestModeEnabled(true);
    QSettings().remove(QStringLiteral("ProcessingCore/Line"));
    stage("QApplication ready");
    mib::test::TempDir dataRoot("processing_core_dialog");
    backend::AppBackend backend;
    stage("initializing AppBackend");
    MIB_REQUIRE(backend.initialize(dataRoot.path().string()),
                "backend initializes with all optional services disabled");
    stage("AppBackend initialized");

    const auto localCore = backend.processing().activeProcessingCoreIdentity();
    stage("constructing ProcessingCoreDialog");
    frontend::ProcessingCoreDialog dialog(backend);
    stage("ProcessingCoreDialog constructed");
    const auto labels = dialog.findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);

    bool renderedLocalCore = false;
    bool renderedRegistryFailure = false;
    for (const auto* label : labels) {
        const QString text = label->text();
        renderedLocalCore = renderedLocalCore ||
                            (text.contains(QStringLiteral("Active core:")) &&
                             text.contains(QString::fromStdString(localCore.version)) &&
                             text.contains(QString::number(localCore.contractVersion)) &&
                             text.contains(QString::number(localCore.engineAbiVersion)) &&
                             text.contains(QString::fromStdString(localCore.source)));
        renderedRegistryFailure = renderedRegistryFailure ||
                                  text.contains(QStringLiteral("must use HTTPS"));
    }

    MIB_EXPECT(renderedLocalCore,
               "dialog renders the local active-core identity before registry success");
    MIB_EXPECT(renderedRegistryFailure,
               "registry failure remains visible alongside the local active core");

    // ADR 0007: the dialog offers both core lines and names the active one.
    bool renderedLine = false;
    for (const auto* label : labels) {
        renderedLine = renderedLine || label->text().contains(QStringLiteral("subtract-ring"));
    }
    MIB_EXPECT(renderedLine, "active-core label names the bundled core's line");
    QComboBox* lineBox = nullptr;
    for (auto* box : dialog.findChildren<QComboBox*>()) {
        if (box->findData(QStringLiteral("absdiff-laplacian")) >= 0) lineBox = box;
    }
    MIB_REQUIRE(lineBox != nullptr, "dialog has a core-line selector");
    MIB_EXPECT(lineBox->count() == 2 &&
                   lineBox->currentData().toString() == QStringLiteral("subtract-ring"),
               "both core lines offered; subtract-ring by default");
    lineBox->setCurrentIndex(lineBox->findData(QStringLiteral("absdiff-laplacian")));
    MIB_EXPECT(QSettings().value(QStringLiteral("ProcessingCore/Line")).toString() ==
                   QStringLiteral("absdiff-laplacian"),
               "selected core line is persisted");
    bool reloadedWithFailure = false;
    for (const auto* label : dialog.findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) {
        reloadedWithFailure = reloadedWithFailure || label->text().contains(QStringLiteral("must use HTTPS"));
    }
    MIB_EXPECT(reloadedWithFailure, "switching line reloads that line's registry");
    QSettings().remove(QStringLiteral("ProcessingCore/Line"));
    return mib::test::exitCode();
}
