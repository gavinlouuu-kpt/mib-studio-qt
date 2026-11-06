// hdf_review_a11y_teardown_test (issue #859)
//
// A UI Automation client (a screen reader, the IME, a test driver) makes Qt
// create one accessible interface per metrics-table cell it visits, each
// holding a QPersistentModelIndex. When the Review tab was destroyed at exit,
// the dying QTableViews released those interfaces one by one, and the cost grew
// faster than the square of the cell count: on the bench the main thread spun
// at 100% for minutes after "AppBackend: shutdown complete" and the process
// never exited (3,300 rows x 24 columns). Plain Qt on Linux, measured for this
// issue: 6,275 cells took 3.9 s, 12,525 took 28.6 s, 25,025 took 284 s.
//
// The tab now resets its metrics models before its children die. This test
// loads a file, walks every cell's accessible interface the way a UIA client
// does, destroys the tab, and requires the teardown to cost no more than the
// walk that created the interfaces (ratio gate with a floor, not a fixed
// time). With the old destructor it takes tens of seconds here.

#include <QAccessible>
#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTableView>

#include <cstdio>
#include <memory>
#include <opencv2/core.hpp>
#include <vector>

#include "backend/app/AppBackend.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "frontend/tabs/HdfReviewTab.h"
#include "frontend/utils/ApplicationSettings.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;

namespace {

void settle(int rounds = 6) {
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, 0);
    }
}

ProcessedFrame frame(uint64_t idx, bool valid) {
    ProcessedFrame f;
    f.index = idx;
    f.timestampNs = (idx + 1) * 1000ULL;
    f.originalImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(static_cast<double>(40 + idx % 200)));
    f.processedImage = cv::Mat(8, 10, CV_8UC1, cv::Scalar(255));
    f.validation.isValid = valid;
    f.validation.objectId = static_cast<int>(idx);
    f.validation.objectCount = 1;
    f.validation.area = 400.0 + static_cast<double>(idx % 97) * 11.0;
    f.validation.deformability = 0.02 + static_cast<double>(idx % 7) * 0.013;
    return f;
}

void writeExperiment(const std::string& path, int validCount, int invalidCount) {
    std::vector<ProcessedFrame> valid;
    std::vector<ProcessedFrame> invalid;
    for (int i = 0; i < validCount; ++i) valid.push_back(frame(static_cast<uint64_t>(i), true));
    for (int i = 0; i < invalidCount; ++i)
        invalid.push_back(frame(static_cast<uint64_t>(validCount + i), false));
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "fixture create");
    MIB_REQUIRE(hdf5.initializeDatasets(), "fixture datasets");
    MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "fixture frames");
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, 10, 8};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 4000, valid.size(), invalid.size(), cfg, roi, nullptr, nullptr),
                "fixture info");
    hdf5.closeFile();
}

// What a UIA client does when it walks the tree: one accessible interface per cell, kept in
// Qt's accessibility cache until the table changes or dies.
int visitCells(QTableView* table) {
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(table);
    if (!iface) return 0;
    const int n = iface->childCount();
    int visited = 0;
    for (int i = 0; i < n; ++i) visited += iface->child(i) ? 1 : 0;
    return visited;
}

} // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("MIB_DISABLED_SERVICES", QByteArrayLiteral("auto_update,autofocus,trigger,syringe_pump"));
    qputenv("MIB_CAMERA_MODE", QByteArrayLiteral("mock"));
    qputenv("MIB_STUDIO_PROCESSING_CORE_BASE_URL", QByteArrayLiteral("http://invalid-registry.example"));
    qputenv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", QByteArrayLiteral("file:///nonexistent/mib-lut-manifest.json"));
    if (!qEnvironmentVariableIsSet("MIB_MOCK_CAMERA_DIR"))
        qputenv("MIB_MOCK_CAMERA_DIR", QByteArrayLiteral("data/mock_frames"));
    // The old teardown of this fixture takes tens of seconds; the gate below fails it well
    // before the watchdog, which is only the backstop.
    mib::test::Watchdog wd(170);
    QApplication app(argc, argv);
    mib::test::TempDir td("hdf_review_a11y_teardown");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString((td.path() / "settings").string()));
    QString err;
    MIB_REQUIRE(frontend::applicationsettings::initialize(&err), "settings init");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");

    constexpr int kValid = 500;
    constexpr int kInvalid = 100;
    const std::string path = (td.path() / "review.h5").string();
    writeExperiment(path, kValid, kInvalid);

    wd.mark("load");
    auto tab = std::make_unique<frontend::HdfReviewTab>(backend);
    tab->resize(1400, 900);
    tab->show();
    settle(4);
    tab->loadHdfFileForTests(QString::fromStdString(path));
    settle(8);
    auto* validTable = tab->findChild<QTableView*>(QStringLiteral("validMetricsTable"));
    auto* invalidTable = tab->findChild<QTableView*>(QStringLiteral("invalidMetricsTable"));
    MIB_REQUIRE(validTable && invalidTable, "metrics tables exist");
    MIB_REQUIRE(validTable->model() && validTable->model()->rowCount() == kValid, "valid rows loaded");
    MIB_REQUIRE(invalidTable->model() && invalidTable->model()->rowCount() == kInvalid, "invalid rows loaded");

    wd.mark("walk");
    QElapsedTimer walkTimer;
    walkTimer.start();
    const int cells = visitCells(validTable) + visitCells(invalidTable);
    const double walkMs = static_cast<double>(walkTimer.nsecsElapsed()) / 1e6;
    MIB_REQUIRE(cells >= (kValid + kInvalid) * 24, "every cell has an accessible interface");

    wd.mark("teardown");
    QElapsedTimer teardownTimer;
    teardownTimer.start();
    tab.reset();
    const double teardownMs = static_cast<double>(teardownTimer.nsecsElapsed()) / 1e6;
    std::fprintf(stderr, "hdf_review_a11y_teardown: %d cell interfaces, walk %.1f ms, teardown %.1f ms\n", cells,
                 walkMs, teardownMs);
    // Linear teardown costs about what the walk did; the old one grew with the square (and worse)
    // of the cell count. 1 s floor for slow sanitizer lanes.
    MIB_EXPECT(teardownMs <= 2.0 * walkMs + 1000.0,
               "Review tab teardown stays linear in the visited cells (#859): " + std::to_string(teardownMs) +
                   " ms for a " + std::to_string(walkMs) + " ms walk");

    backend.shutdown();
    return mib::test::exitCode();
}
