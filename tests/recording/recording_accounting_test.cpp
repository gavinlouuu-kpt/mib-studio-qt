// recording_accounting_test (issue #367)
//
// 1. Pure reconciliation: every completion state is derived from the counters
//    and an unreconciled snapshot can never be Complete.
// 2. Mock-camera raw recording through AppBackend with an injected faulting
//    processing kernel: processing failures are counted as ProcessingFailed
//    (never as empty/filtered), a slow consumer against a tiny FrameStore
//    produces StoreOverwritten (IncompleteLoss), a clean run is Complete, and
//    the accounting survives an HDF5 close/reopen byte-for-byte.
// 3. Legacy files without accounting read back as Unknown, never
//    reinterpreted.

#include "backend/app/AppBackend.h"
#include "backend/camera/mock/MockCamera.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/RecordingAccounting.h"
#include "backend/services/CaptureService.h"

#include "support/assert.h"
#include "support/fault_kernel.h"
#include "support/frames.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <QCoreApplication>

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

namespace rec = backend::recording;

namespace {
bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void setEnv(const char* k, const std::string& v)
{
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

// Frames: even indices carry a ring (non-empty), odd indices are black (empty).
bool writeMixedFrames(const std::filesystem::path& dir, int count)
{
    std::filesystem::create_directories(dir);
    for (int i = 0; i < count; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05d.png", i);
        cv::Mat m = (i % 2 == 0) ? mib::test::ringFrame(96, 96, static_cast<uint64_t>(i))
                                 : cv::Mat(96, 96, CV_8UC1, cv::Scalar(0));
        if (!cv::imwrite((dir / name).string(), m)) return false;
    }
    return true;
}

rec::RecordingAccountingSnapshot reopenAccounting(const std::string& path, bool& ok)
{
    backend::services::Hdf5Service reader;
    rec::RecordingAccountingSnapshot a;
    ok = reader.loadFile(path) && reader.readRunAccounting(a);
    reader.closeFile();
    return a;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    mib::test::Watchdog wd(60);

    // ---- 1. Pure reconciliation ------------------------------------------------
    {
        wd.mark("pure");
        rec::RecordingAccountingSnapshot s;
        s.admitted = 10; s.empty = 4; s.processed = 6; s.persistenceAdmitted = 6; s.persistenceCommitted = 6;
        auto r = rec::reconcile(s);
        MIB_EXPECT(r.reconciled && r.completion == rec::RunCompletionState::Complete, "clean run Complete");

        auto loss = s; loss.processed = 5; loss.processingFailed = 1; loss.persistenceAdmitted = 5; loss.persistenceCommitted = 5;
        r = rec::reconcile(loss);
        MIB_EXPECT(r.reconciled && r.completion == rec::RunCompletionState::IncompleteLoss, "processing failure -> IncompleteLoss");

        auto ow = s; ow.admitted = 12; ow.storeOverwritten = 2;
        r = rec::reconcile(ow);
        MIB_EXPECT(r.completion == rec::RunCompletionState::IncompleteLoss, "store overwrite -> IncompleteLoss");

        auto pf = s; pf.persistenceCommitted = 5; pf.persistenceFailed = 1;
        r = rec::reconcile(pf);
        MIB_EXPECT(r.completion == rec::RunCompletionState::Failed, "persistence failure -> Failed");

        auto pend = s; pend.persistenceCommitted = 4; pend.persistencePendingAtStop = 2;
        r = rec::reconcile(pend);
        MIB_EXPECT(r.completion == rec::RunCompletionState::IntentionallyPartial, "pending at stop -> IntentionallyPartial");

        auto policy = s; policy.policyAllowsDrops = true;
        r = rec::reconcile(policy);
        MIB_EXPECT(r.completion == rec::RunCompletionState::IntentionallyPartial, "declared drop policy -> IntentionallyPartial");

        auto bad = s; bad.empty = 3; // frame terms no longer sum to admitted
        r = rec::reconcile(bad);
        MIB_EXPECT(!r.reconciled && r.completion == rec::RunCompletionState::Failed,
                   "unreconciled accounting can never be Complete");
        auto badP = s; badP.persistenceCommitted = 5;
        r = rec::reconcile(badP);
        MIB_EXPECT(!r.reconciled && r.completion == rec::RunCompletionState::Failed, "persistence mismatch -> Failed");

        auto fatal = s; fatal.fatalError = true; fatal.fatalMessage = "disk";
        r = rec::reconcile(fatal);
        MIB_EXPECT(r.completion == rec::RunCompletionState::Failed && r.completionReason == "disk", "fatal -> Failed");

        // Accounting object: admit tracks sequence gaps; admitLost adds terms.
        rec::RecordingAccounting acc;
        acc.reset(7, false);
        acc.admit(10); acc.admit(11); acc.admit(15); acc.count(rec::FrameOutcome::Empty, 3);
        acc.admitLost(2, rec::FrameOutcome::StoreOverwritten);
        auto snap = rec::reconcile(acc.snapshot());
        MIB_EXPECT(snap.admitted == 5 && snap.sequenceGaps == 1 && snap.sequenceGapFrames == 3, "sequence gap tracked");
        MIB_EXPECT(snap.firstFrameIndex == 10 && snap.lastFrameIndex == 15 && snap.sessionGeneration == 7, "range + generation");
        MIB_EXPECT(snap.reconciled && snap.completion == rec::RunCompletionState::IncompleteLoss, "gap + overwrite -> loss");
    }

    // ---- 2. Mock recording runs through AppBackend ----------------------------
    mib::test::TempDir td("recording_accounting");
    const auto frameDir = td.path() / "frames";
    MIB_REQUIRE(writeMixedFrames(frameDir, 40), "write mixed frames");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_MOCK_CAMERA_DIR", frameDir.string());
    setEnv("MIB_MOCK_CAMERA_INTERVAL_MS", "1");
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize(td.path().string()), "backend init");
    auto kernel = std::make_shared<mib::test::FaultKernel>();
    std::string activationError;
    MIB_REQUIRE(backend.processing().activateProcessingKernel(kernel, &activationError),
                "activate fault kernel: " + activationError);
    {
        auto cfg = backend.processing().getProcessingConfig();
        cfg.empty_frame_pixel_threshold = 1;
        cfg.bg_subtract_threshold = 100;
        backend.processing().setProcessingConfig(cfg);
    }

    auto runRecording = [&](const char* label, std::chrono::milliseconds duration) {
        wd.mark(label);
        const std::string path = (td.path() / (std::string(label) + ".h5")).string();
        MIB_REQUIRE(backend.capture().start(), std::string(label) + ": capture start");
        MIB_REQUIRE(waitFor([&] { return backend.capture().stats().framesProcessed.load() > 5; },
                            std::chrono::seconds(10)),
                    std::string(label) + ": frames flowing");
        MIB_REQUIRE(backend.startFrameRecording(path), std::string(label) + ": recording start");
        std::this_thread::sleep_for(duration);
        backend.stopFrameRecording();
        backend.capture().stop();
        return path;
    };

    // 2a. Clean run: only empty + processed; persisted == processed; Complete.
    {
        const std::string path = runRecording("clean", std::chrono::milliseconds(800));
        const auto a = backend.recordingAccounting();
        std::fprintf(stderr, "clean: admitted=%llu empty=%llu processed=%llu failed=%llu ow=%llu "
                     "persisted=%llu/%llu completion=%s reason=%s\n",
                     (unsigned long long)a.admitted, (unsigned long long)a.empty,
                     (unsigned long long)a.processed, (unsigned long long)a.processingFailed,
                     (unsigned long long)a.storeOverwritten, (unsigned long long)a.persistenceCommitted,
                     (unsigned long long)a.persistenceAdmitted, rec::toString(a.completion),
                     a.completionReason.c_str());
        MIB_EXPECT(a.admitted > 10, "frames admitted");
        MIB_EXPECT(a.empty > 0 && a.processed > 0, "both empty and processed frames seen");
        MIB_EXPECT(a.processingFailed == 0 && a.storeOverwritten == 0, "no loss in the clean run");
        MIB_EXPECT(a.reconciled, "clean run reconciles");
        MIB_EXPECT(a.completion == rec::RunCompletionState::Complete, "clean run Complete");
        MIB_EXPECT(a.persistenceAdmitted == a.processed && a.persistenceCommitted == a.processed,
                   "every processed frame persisted");
        MIB_EXPECT(a.persistenceCommitted == backend.frameRecordingCount(), "written count agrees");
        MIB_EXPECT(a.empty == backend.frameRecordingFiltered(), "filtered count agrees with empty");
        MIB_EXPECT(a.sessionGeneration == backend.capture().lifecycleSnapshot().generation,
                   "accounting tagged with the capture session generation");
        bool ok = false;
        const auto reread = reopenAccounting(path, ok);
        MIB_REQUIRE(ok, "accounting readable after reopen");
        MIB_EXPECT(reread.admitted == a.admitted && reread.empty == a.empty &&
                       reread.processed == a.processed && reread.persistenceCommitted == a.persistenceCommitted &&
                       reread.completion == a.completion && reread.reconciled == a.reconciled &&
                       reread.sessionGeneration == a.sessionGeneration &&
                       reread.firstFrameIndex == a.firstFrameIndex && reread.lastFrameIndex == a.lastFrameIndex,
                   "HDF5 round-trip preserves the accounting");
        MIB_EXPECT(reread.schemaVersion == rec::RecordingAccountingSnapshot::kSchemaVersion, "schema version stored");
    }

    // 2b. Processing failure: every 3rd empty check fails -> ProcessingFailed,
    //     NOT filtered/empty; run is IncompleteLoss and still reconciles.
    {
        kernel->mode = static_cast<int>(mib::test::FaultKernel::Mode::FailEmptyCheck);
        kernel->failPeriod = 3;
        const std::string path = runRecording("processing_failure", std::chrono::milliseconds(800));
        kernel->failPeriod = 0;
        const auto a = backend.recordingAccounting();
        std::fprintf(stderr, "fail: admitted=%llu empty=%llu processed=%llu failed=%llu injected=%llu completion=%s\n",
                     (unsigned long long)a.admitted, (unsigned long long)a.empty,
                     (unsigned long long)a.processed, (unsigned long long)a.processingFailed,
                     (unsigned long long)kernel->injectedFailures.load(), rec::toString(a.completion));
        MIB_EXPECT(a.processingFailed > 0, "processing failures counted");
        MIB_EXPECT(a.processingFailed == kernel->injectedFailures.load(), "exactly the injected failures");
        MIB_EXPECT(a.empty + a.processed + a.processingFailed == a.admitted, "frame terms exact");
        MIB_EXPECT(a.reconciled, "faulty run still reconciles");
        MIB_EXPECT(a.completion == rec::RunCompletionState::IncompleteLoss, "processing failure -> IncompleteLoss, never Complete");
        MIB_EXPECT(backend.frameRecordingFiltered() == a.empty, "filtered count excludes failures");
        bool ok = false;
        const auto reread = reopenAccounting(path, ok);
        MIB_REQUIRE(ok, "reopen");
        MIB_EXPECT(reread.processingFailed == a.processingFailed &&
                       reread.completion == rec::RunCompletionState::IncompleteLoss,
                   "failure category survives reopen");
        kernel->injectedFailures = 0;
    }

    // 2c. Exceptions thrown by the core are ProcessingFailed too.
    {
        kernel->mode = static_cast<int>(mib::test::FaultKernel::Mode::ThrowEmptyCheck);
        kernel->failPeriod = 4;
        runRecording("processing_throw", std::chrono::milliseconds(500));
        kernel->failPeriod = 0;
        const auto a = backend.recordingAccounting();
        MIB_EXPECT(a.processingFailed == kernel->injectedFailures.load() && a.processingFailed > 0,
                   "thrown exceptions counted as processing failures");
        MIB_EXPECT(a.reconciled && a.completion == rec::RunCompletionState::IncompleteLoss, "throw run reconciles as loss");
        kernel->injectedFailures = 0;
        kernel->mode = static_cast<int>(mib::test::FaultKernel::Mode::Passthrough);
    }

    // 2d. Slow consumer vs tiny ring: overwritten frames are explicit loss.
    {
        wd.mark("overwrite");
        MIB_REQUIRE(backend.getFrameStore()->resize(6), "shrink ring");
        kernel->delayMs = 8; // each classification slower than 6 frames' arrival at 1 ms
        const std::string path = runRecording("overwrite", std::chrono::milliseconds(600));
        kernel->delayMs = 0;
        MIB_REQUIRE(backend.getFrameStore()->resize(5000), "restore ring");
        const auto a = backend.recordingAccounting();
        std::fprintf(stderr, "overwrite: admitted=%llu empty=%llu processed=%llu overwritten=%llu gaps=%llu completion=%s\n",
                     (unsigned long long)a.admitted, (unsigned long long)a.empty,
                     (unsigned long long)a.processed, (unsigned long long)a.storeOverwritten,
                     (unsigned long long)a.sequenceGaps, rec::toString(a.completion));
        MIB_EXPECT(a.storeOverwritten > 0, "ring overwrite counted as explicit store loss");
        MIB_EXPECT(a.reconciled, "overwrite run reconciles");
        MIB_EXPECT(a.completion == rec::RunCompletionState::IncompleteLoss, "overwrite -> IncompleteLoss");
        MIB_EXPECT(a.processingFailed == 0, "no processing failures in the overwrite run");
        bool ok = false;
        const auto reread = reopenAccounting(path, ok);
        MIB_REQUIRE(ok, "reopen overwrite");
        MIB_EXPECT(reread.storeOverwritten == a.storeOverwritten, "overwrite count persisted");
    }

    // ---- 3. Legacy file (no accounting) reads as Unknown ------------------------
    {
        wd.mark("legacy");
        const std::string legacy = (td.path() / "legacy.h5").string();
        {
            backend::services::Hdf5Service w;
            MIB_REQUIRE(w.openFile(legacy), "open legacy");
            MIB_REQUIRE(w.initializeRecordingDatasets(), "init datasets");
            MIB_REQUIRE(w.writeRecordingInfo(1, 2, 0, 0), "legacy recording info");
            w.closeFile();
        }
        bool ok = true;
        const auto a = reopenAccounting(legacy, ok);
        MIB_EXPECT(!ok, "legacy file has no accounting");
        MIB_EXPECT(a.completion == rec::RunCompletionState::Unknown, "legacy completion is Unknown, not Complete");
    }

    backend.shutdown();
    return mib::test::exitCode();
}
