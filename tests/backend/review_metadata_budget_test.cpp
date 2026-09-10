#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/recording/Hdf5Service.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

int main() {
    mib::test::Watchdog watchdog;
    mib::test::TempDir tmp("review_budget");
    const auto source = tmp / "source.h5";
    {
        backend::services::Hdf5Service writer;
        MIB_REQUIRE(writer.openFile(source.string()), "fixture");
        MIB_REQUIRE(writer.initializeRecordingDatasets(), "datasets");
        MIB_REQUIRE(
            writer.appendRecordingFrames({cv::Mat(2, 2, CV_8UC1, cv::Scalar(7))}, {{31, 42, 2, 2}}),
            "raw metadata");
        MIB_REQUIRE(writer.writeRecordingInfo(42, 42, 1, 0), "recording info");
    }
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize((tmp / "app").string()), "startup");
    MIB_REQUIRE(facade.dispatch(backend::bridge::RecordingLoadCommand{source.string(), true}).ok,
                "open");
    std::vector<backend::bridge::MonitoringObjectRow> rows;
    uint64_t total = 0;
    MIB_EXPECT(!facade.fetchReviewMetricsPage(true, 0, UINT64_MAX, rows, total),
               "unbounded request must fail before reading/allocating metadata");
    facade.shutdown();
    return mib::test::exitCode();
}
