// client_output_path_test (#762, and F2 of #763)
//
// A relative output path a client gives a writer resolves under the storage root (the data
// directory, fetch_instrument_status storage.path), never under the server's working directory:
//  1. app::resolveClientPath, the one rule: relative, nested relative, `..` that stays inside,
//     `..` that leaves the root, a sibling directory whose name starts with the root's (compared
//     by component, not as a string), absolute (unchanged), a symlinked parent or file that
//     resolves outside, a dangling symlink, a symlink that stays inside, empty, no root configured
//     (unchanged: today's behaviour), the root itself, a root given through a symlink, a non-ASCII
//     root and paths (UTF-8 in and out; an absolute one byte for byte).
//  2. Through the facade, as the bridge drives it (readiness for the path, then Start), with a
//     mock camera and the process's working directory moved to an empty folder:
//     - fetch_instrument_status storage.path is the absolute storage root;
//     - the readiness probes for a relative path run in the resolved directory, and nothing is
//       written in the working directory;
//     - Start with "experiment.h5" writes <root>/experiment.h5; the reply, the status, the run
//       and the persisted snapshot carry that absolute path; a nested relative path and a path
//       without the extension (readiness and Start agree: no stale generation) too;
//     - `..` escaping the root, a symlinked parent escaping it and an empty path are
//       InvalidRequest with a plain reason, open nothing, and readiness fails storage.output
//       without probing;
//     - an absolute path outside the root keeps today's behaviour;
//     - non-ASCII relative and absolute paths: UTF-8 in the reply, status and snapshot, and the
//       file where that UTF-8 path says;
//     - raw recording, the review metrics export, Regenerate masks and the preview buffer save
//       resolve their client paths the same way (relative under the root, escape refused);
//  3. F2: the storage.hdf5 gate and Start's refusal say how to free the file a review holds
//     ("Close the file in Review (Review → Close File)"); while raw recording holds the file
//     they point at the recording instead.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/app/RecordingTarget.h"
#include "backend/recording/Hdf5Service.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace bridge = backend::bridge;
namespace app = backend::app;
namespace fs = std::filesystem;
using app::ExperimentStartOutcome;
using app::GateStatus;

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string startName(const std::optional<ExperimentStartOutcome>& o) { return o ? app::toString(*o) : "none"; }

GateStatus statusOf(const app::ExperimentReadinessSnapshot& r, const char* id)
{
    const auto* g = r.gate(id);
    return g ? g->status : GateStatus::Unavailable;
}

size_t entries(const fs::path& dir)
{
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

// Set the directory's modification time an hour back and return it: a file created or removed in
// it afterwards gives another time.
fs::file_time_type age(const fs::path& dir)
{
    fs::last_write_time(dir, fs::last_write_time(dir) - std::chrono::hours(1));
    return fs::last_write_time(dir);
}

bool makeSymlink(const fs::path& target, const fs::path& link, bool directory)
{
    std::error_code ec;
    if (directory) fs::create_directory_symlink(target, link, ec);
    else fs::create_symlink(target, link, ec);
    return !ec;
}

// Start through the facade the way the bridge does (shim experiment_start): evaluate readiness for
// the path, present that generation, then dispatch Start.
bridge::BackendCommandResult startRun(bridge::BackendFacade& facade, const std::string& path)
{
    app::ExperimentReadinessSnapshot readiness;
    bridge::ExperimentCommand start;
    start.action = bridge::ExperimentCommandAction::Start;
    start.outputPath = path;
    if (facade.fetchExperimentReadiness(readiness, path)) start.readinessGeneration = readiness.generation;
    return facade.dispatch(start);
}

bool stopRun(bridge::BackendFacade& facade, app::ExperimentCoordinator& coordinator)
{
    bridge::ExperimentCommand stop;
    stop.action = bridge::ExperimentCommandAction::Stop;
    if (!facade.dispatch(stop).ok) return false;
    return waitFor([&] { return coordinator.status().terminal && coordinator.state() == app::ExperimentRunState::Idle; },
                   std::chrono::seconds(20));
}

// ---- 1. The rule itself ------------------------------------------------------------------------
void resolverCases(const fs::path& base)
{
    const fs::path data = base / "data";
    const fs::path outside = base / "outside";
    fs::create_directories(data);
    fs::create_directories(outside);
    fs::create_directories(base / "data2");
    const std::string root = app::storageRootOf(data.string());
    const fs::path R(root);
    MIB_REQUIRE(R.is_absolute() && fs::equivalent(R, data), "the storage root is the data dir, absolute: " + root);
    MIB_EXPECT(app::storageRootOf((data / "").string()) == root, "a trailing separator gives the same root");
    MIB_EXPECT(app::storageRootOf("").empty(), "no data dir: no root");

    auto ok = [&](const std::string& requested, const fs::path& expected, const char* what) {
        const auto r = app::resolveClientPath(requested, root);
        MIB_EXPECT(r.ok && r.path == expected.string(),
                   std::string(what) + ": '" + requested + "' -> '" + r.path + "' (" + r.reason + "), expected " +
                       expected.string());
    };
    auto refused = [&](const std::string& requested, const char* part, const char* what) {
        const auto r = app::resolveClientPath(requested, root);
        MIB_EXPECT(!r.ok && contains(r.reason, part),
                   std::string(what) + ": '" + requested + "' must be refused with '" + part + "', got ok=" +
                       std::to_string(r.ok) + " path='" + r.path + "' reason='" + r.reason + "'");
    };

    ok("experiment.h5", R / "experiment.h5", "relative");
    ok("runs/day 1/exp.h5", R / "runs" / "day 1" / "exp.h5", "nested relative (parents not created yet)");
    ok("runs/../inside.h5", R / "inside.h5", "`..` that stays inside");
    ok("./dot.h5", R / "dot.h5", "a leading ./");
    ok(".", R, "the root itself (a folder writer writes into it)");
    refused("../escape.h5", "leaves the data directory", "`..` that leaves the root");
    refused("runs/../../escape.h5", "leaves the data directory", "`..` deeper that leaves the root");
    refused("../data2/run.h5", "leaves the data directory", "a sibling named like the root (component-wise, not a string prefix)");
    refused("", "no output path given", "empty");

    // Absolute: unchanged, byte for byte (the desktop's native dialogs).
    const std::string abs = (outside / "abs.h5").string();
    MIB_EXPECT(app::resolveClientPath(abs, root).ok && app::resolveClientPath(abs, root).path == abs,
               "an absolute path outside the root is unchanged");
    const std::string absDots = (data / ".." / "outside" / "x.h5").string();
    MIB_EXPECT(app::resolveClientPath(absDots, root).path == absDots, "an absolute path is not normalized either");

    // No root configured: today's behaviour, relative to the working directory.
    {
        const auto r = app::resolveClientPath("rel.h5", "");
        MIB_EXPECT(r.ok && r.path == "rel.h5", "no root: the relative path is unchanged");
    }

    // Non-ASCII (UTF-8 in, UTF-8 out; on MSVC path::string() would give the ANSI code page): a root
    // and a relative path with accents, and an absolute path that comes back byte for byte.
    {
        const std::string dataName = "d\xC3\xA4t\xC3\xA4";          // "dätä"
        const std::string cafe = "caf\xC3\xA9";                     // "café"
        const fs::path dataU = base / app::utf8ToPath(dataName);
        fs::create_directories(dataU);
        const std::string rootU = app::storageRootOf(app::pathToUtf8(dataU));
        MIB_EXPECT(rootU == app::pathToUtf8(fs::weakly_canonical(dataU)) && contains(rootU, dataName),
                   "a non-ASCII root is UTF-8: " + rootU);
        MIB_EXPECT(app::recordingTarget(rootU).path == rootU, "recordingTarget reports it in UTF-8 (storage.path)");
        const auto rel = app::resolveClientPath(cafe + "/run \xC3\xA9.h5", rootU);
        MIB_EXPECT(rel.ok && rel.path == app::pathToUtf8(app::utf8ToPath(rootU) / app::utf8ToPath(cafe) /
                                                         app::utf8ToPath("run \xC3\xA9.h5")),
                   "a non-ASCII relative path under a non-ASCII root, in UTF-8: " + rel.path + " " + rel.reason);
        const auto esc = app::resolveClientPath("../x.h5", rootU);
        MIB_EXPECT(!esc.ok && contains(esc.reason, rootU), "the refusal names the root in UTF-8: " + esc.reason);
        const std::string absU = app::pathToUtf8(outside / app::utf8ToPath("na\xC3\xAFve") / app::utf8ToPath("abs \xC3\xA9.h5"));
        const auto abs8 = app::resolveClientPath(absU, rootU);
        MIB_EXPECT(abs8.ok && abs8.path == absU, "a non-ASCII absolute path comes back byte for byte");
    }

    // Symlinks (skipped where the platform refuses to create them).
    if (makeSymlink(outside, data / "out_link", true)) {
        refused("out_link/run.h5", "leaves the data directory", "a symlinked parent that resolves outside");
        { std::ofstream f(outside / "target.h5"); f << "x"; }
        MIB_REQUIRE(makeSymlink(outside / "target.h5", data / "leaf.h5", false), "file symlink");
        refused("leaf.h5", "leaves the data directory", "an existing file symlink that resolves outside");
        MIB_REQUIRE(makeSymlink(outside / "missing.h5", data / "dangling.h5", false), "dangling file symlink");
        refused("dangling.h5", "points nowhere", "a dangling symlink as the file");
        MIB_REQUIRE(makeSymlink(outside / "no_dir", data / "dangling_dir", true), "dangling dir symlink");
        refused("dangling_dir/run.h5", "points nowhere", "a dangling symlink as a parent");
        fs::create_directories(data / "real");
        MIB_REQUIRE(makeSymlink(data / "real", data / "alias", true), "inner symlink");
        ok("alias/run.h5", R / "real" / "run.h5", "a symlink that stays inside (the resolved path is returned)");
        // A root given through a symlink is the same root.
        MIB_REQUIRE(makeSymlink(data, base / "root_link", true), "root symlink");
        const auto viaLink = app::resolveClientPath("x.h5", (base / "root_link").string());
        MIB_EXPECT(viaLink.ok && viaLink.path == (R / "x.h5").string(), "a symlinked root resolves to the real one: " + viaLink.path);
        MIB_EXPECT(app::storageRootOf((base / "root_link").string()) == root, "storageRootOf resolves the root's symlink");
    } else {
        std::fprintf(stderr, "note: symlinks unavailable here; symlink cases skipped\n");
    }
}
} // namespace

int main()
{
    mib::test::Watchdog wd(90);
    mib::test::TempDir td("mib_client_output_path");
    const fs::path base = fs::weakly_canonical(td.path());

    wd.mark("resolver");
    {
        // F2: the instruction itself, pinned once here (every other test uses the constant).
        MIB_EXPECT(std::string(app::kCloseReviewFile) == "Close the file in Review (Review \xE2\x86\x92 Close File)",
                   "the F2 instruction text");
    }
    resolverCases(base / "unit");

    // ---- 2. Through the facade -------------------------------------------------------------
    const auto mockDir = base / "mock_frames";
    fs::create_directories(mockDir);
    MIB_REQUIRE(cv::imwrite((mockDir / "frame_000.tiff").string(), cv::Mat(96, 512, CV_8UC1, cv::Scalar(180))),
                "mock frame fixture");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_STUDIO_EMODULUS_LUT_CACHE_DIR", (base / "lut-cache").string().c_str());
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    // The server's working directory: an empty folder of our own. Before #762 a relative path
    // landed here.
    const fs::path cwd = base / "cwd";
    fs::create_directories(cwd);
    const fs::path previousCwd = fs::current_path();
    fs::current_path(cwd);

    const fs::path dataDir = base / "data";
    const fs::path outside = base / "outside";
    fs::create_directories(outside);
    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        std::mutex eventsMutex;
        std::vector<bridge::OperationStatusEvent> operations;
        facade.setEventSink([&](const bridge::BackendEvent& event) {
            if (const auto* o = std::get_if<bridge::OperationStatusEvent>(&event)) {
                std::scoped_lock lock(eventsMutex);
                operations.push_back(*o);
            }
        });
        auto terminalOf = [&](std::uint64_t id) -> std::optional<bridge::OperationStatusEvent> {
            std::scoped_lock lock(eventsMutex);
            for (const auto& o : operations)
                if (o.operationId == id && o.state != bridge::BackendOperationState::Started &&
                    o.state != bridge::BackendOperationState::Progress)
                    return o;
            return std::nullopt;
        };
        MIB_REQUIRE(facade.initialize(dataDir.string()), "facade initialize");
        auto& coordinator = backendApp.experiment();
        const std::string root = backendApp.storageRoot();
        const fs::path R(root);
        MIB_REQUIRE(R.is_absolute() && fs::equivalent(R, dataDir), "storage root = the data dir: " + root);

        wd.mark("instrument status");
        {
            const auto status = nlohmann::json::parse(facade.fetchInstrumentStatusJson());
            MIB_EXPECT(status.contains("storage") && status["storage"].value("path", std::string()) == root,
                       "fetch_instrument_status storage.path is the absolute storage root: " + status.dump());
        }

        wd.mark("camera");
        {
            bridge::CameraCommand configure;
            configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
            configure.mockFrameDirectory = mockDir.string();
            configure.mockFrameIntervalMs = 2;
            configure.mockLoopFiles = true;
            MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
            bridge::CameraCommand startCapture;
            startCapture.action = bridge::CameraCommandAction::StartCapture;
            MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture start");
            MIB_REQUIRE(waitFor(
                            [&] {
                                app::ExperimentReadinessSnapshot readiness;
                                return facade.fetchExperimentReadiness(readiness, "experiment.h5") && readiness.ready;
                            },
                            std::chrono::seconds(15)),
                        "readiness passes for a relative path with a running mock camera");
        }
        MIB_EXPECT(entries(cwd) == 0, "nothing was written in the working directory by the readiness polls");

        // ---- the readiness probe runs in the resolved directory ----
        wd.mark("probe location");
        {
            fs::create_directories(R / "probes");
            const auto agedProbes = age(R / "probes");
            const auto agedCwd = age(cwd);
            app::ExperimentReadinessSnapshot r;
            MIB_REQUIRE(facade.fetchExperimentReadiness(r, "probes/probe_here.h5"), "readiness");
            MIB_EXPECT(statusOf(r, "storage.output") == GateStatus::Pass && statusOf(r, "storage.roundtrip") == GateStatus::Pass,
                       "the relative destination passes");
            const auto* out = r.gate("storage.output");
            MIB_EXPECT(out && out->detail == (R / "probes" / "probe_here.h5").string(),
                       "the gate names the resolved path: " + (out ? out->detail : std::string("-")));
            MIB_EXPECT(r.candidate.outputPath == (R / "probes" / "probe_here.h5").string(),
                       "the candidate carries the resolved path: " + r.candidate.outputPath);
            MIB_EXPECT(fs::last_write_time(R / "probes") != agedProbes, "the probes ran in the resolved directory");
            MIB_EXPECT(entries(R / "probes") == 0, "and left nothing there");
            MIB_EXPECT(fs::last_write_time(cwd) == agedCwd && entries(cwd) == 0,
                       "no probe file was created in the working directory");
        }

        // ---- Start with the prompt's old default ----
        wd.mark("relative start");
        const fs::path runFile = R / "experiment.h5";
        {
            const auto r = startRun(facade, "experiment.h5");
            MIB_REQUIRE(r.ok && r.experimentStartOutcome == ExperimentStartOutcome::Started,
                        "relative start -> Started, got " + startName(r.experimentStartOutcome) + ": " + r.message);
            MIB_EXPECT(r.message == "experiment started: " + runFile.string(), "the reply carries the resolved path: " + r.message);
            MIB_EXPECT(coordinator.status().outputPath == runFile.string(),
                       "the status carries the resolved path: " + coordinator.status().outputPath);
            const auto run = coordinator.activeRun();
            MIB_EXPECT(run && run->outputPath == runFile.string(), "the frozen run carries the resolved path");
            MIB_EXPECT(fs::exists(runFile), "the run file is under the storage root");
            MIB_EXPECT(!fs::exists(cwd / "experiment.h5") && entries(cwd) == 0, "and not in the working directory");
            MIB_REQUIRE(stopRun(facade, coordinator), "the run finalizes");
            MIB_EXPECT(coordinator.status().outputPath == runFile.string(), "the terminal status keeps the resolved path");
            backend::services::Hdf5Service reader;
            std::string runJson;
            const bool persisted = reader.loadFile(runFile.string()) && reader.readRunSnapshotJson(runJson);
            reader.closeFile();
            MIB_EXPECT(persisted, "the run file under the root holds its snapshot");
            const auto snapshot = persisted ? nlohmann::json::parse(runJson) : nlohmann::json::object();
            MIB_EXPECT(snapshot.value("output_path", std::string()) == runFile.string(),
                       "the snapshot output_path is the resolved absolute path: " + snapshot.value("output_path", std::string()));
        }

        wd.mark("nested and extensionless starts");
        {
            const fs::path nested = R / "runs" / "2026-10-21" / "exp.h5";
            const auto r = startRun(facade, "runs/2026-10-21/exp.h5");
            MIB_EXPECT(r.ok && coordinator.status().outputPath == nested.string() && fs::exists(nested),
                       "a nested relative path lands under the root: " + r.message);
            if (r.ok) MIB_REQUIRE(stopRun(facade, coordinator), "nested run finalizes");
            // Readiness and Start accept the path the same way (the ".h5" too), so the generation the
            // readiness gave is the one Start sees.
            const auto bare = startRun(facade, "noext");
            MIB_EXPECT(bare.ok && coordinator.status().outputPath == (R / "noext.h5").string(),
                       "a path without the extension: " + startName(bare.experimentStartOutcome) + ": " + bare.message);
            if (bare.ok) MIB_REQUIRE(stopRun(facade, coordinator), "extensionless run finalizes");
        }

        wd.mark("refused starts");
        {
            const auto agedCwd = age(cwd);
            const auto agedBase = age(base);
            const auto escape = startRun(facade, "../escape.h5");
            MIB_EXPECT(!escape.ok && escape.experimentStartOutcome == ExperimentStartOutcome::InvalidRequest &&
                           contains(escape.message, "leaves the data directory") && contains(escape.message, root),
                       "`..` escaping the root -> InvalidRequest with a plain reason, got " +
                           startName(escape.experimentStartOutcome) + ": " + escape.message);
            if (escape.ok) MIB_REQUIRE(stopRun(facade, coordinator), "(a wrongly started run is stopped)");
            MIB_EXPECT(!fs::exists(base / "escape.h5"), "nothing was written outside the root");
            app::ExperimentReadinessSnapshot r;
            MIB_REQUIRE(facade.fetchExperimentReadiness(r, "../escape.h5"), "readiness");
            const auto* out = r.gate("storage.output");
            MIB_EXPECT(!r.ready && out && out->status == GateStatus::Fail && contains(out->reason, "leaves the data directory") &&
                           r.gate("storage.roundtrip") == nullptr,
                       "readiness fails storage.output for it and probes nothing");
            MIB_EXPECT(fs::last_write_time(base) == agedBase && fs::last_write_time(cwd) == agedCwd,
                       "no probe outside the root, none in the working directory");

            if (makeSymlink(outside, R / "out_link", true)) {
                const auto viaLink = startRun(facade, "out_link/run.h5");
                MIB_EXPECT(!viaLink.ok && viaLink.experimentStartOutcome == ExperimentStartOutcome::InvalidRequest &&
                               contains(viaLink.message, "leaves the data directory"),
                           "a symlinked parent escaping the root -> InvalidRequest, got " +
                               startName(viaLink.experimentStartOutcome) + ": " + viaLink.message);
                if (viaLink.ok) MIB_REQUIRE(stopRun(facade, coordinator), "(a wrongly started run is stopped)");
                MIB_EXPECT(!fs::exists(outside / "run.h5"), "nothing written through the link");
            }
            const auto empty = startRun(facade, "");
            MIB_EXPECT(!empty.ok && empty.experimentStartOutcome == ExperimentStartOutcome::InvalidRequest &&
                           contains(empty.message, "no output path given"),
                       "an empty path -> InvalidRequest, got " + startName(empty.experimentStartOutcome) + ": " + empty.message);
            MIB_EXPECT(!fs::exists(cwd / ".h5") && !fs::exists(R / ".h5"), "the empty path opened nothing");
            MIB_EXPECT(coordinator.state() == app::ExperimentRunState::Idle && !coordinator.activeRun(),
                       "refused starts leave no run");
        }

        wd.mark("absolute start");
        {
            const std::string abs = (outside / "abs_run.h5").string();
            const auto r = startRun(facade, abs);
            MIB_EXPECT(r.ok && coordinator.status().outputPath == abs && fs::exists(abs),
                       "an absolute path outside the root keeps today's behaviour: " + r.message);
            if (r.ok) MIB_REQUIRE(stopRun(facade, coordinator), "absolute run finalizes");
        }

        // Non-ASCII through Start (#762, review round 1): the reply, the status and the snapshot are
        // UTF-8, and the file is where that UTF-8 path says.
        wd.mark("non-ASCII starts");
        {
            const std::string relU = "caf\xC3\xA9/run \xC3\xA9.h5";
            const std::string expected =
                app::pathToUtf8(R / app::utf8ToPath("caf\xC3\xA9") / app::utf8ToPath("run \xC3\xA9.h5"));
            const auto r = startRun(facade, relU);
            MIB_EXPECT(r.ok && r.message == "experiment started: " + expected && coordinator.status().outputPath == expected,
                       "a non-ASCII relative path: " + r.message);
            if (r.ok) MIB_REQUIRE(stopRun(facade, coordinator), "non-ASCII relative run finalizes");
            MIB_EXPECT(fs::exists(app::utf8ToPath(expected)), "the file is at the UTF-8 path");
            backend::services::Hdf5Service reader;
            std::string runJson;
            const bool persisted = reader.loadFile(expected) && reader.readRunSnapshotJson(runJson);
            reader.closeFile();
            MIB_EXPECT(persisted && nlohmann::json::parse(runJson).value("output_path", std::string()) == expected,
                       "the snapshot records the UTF-8 path");

            const std::string absU =
                app::pathToUtf8(outside / app::utf8ToPath("na\xC3\xAFve") / app::utf8ToPath("abs \xC3\xA9.h5"));
            const auto a = startRun(facade, absU);
            MIB_EXPECT(a.ok && coordinator.status().outputPath == absU,
                       "a non-ASCII absolute path keeps its bytes: " + a.message);
            if (a.ok) MIB_REQUIRE(stopRun(facade, coordinator), "non-ASCII absolute run finalizes");
            MIB_EXPECT(fs::exists(app::utf8ToPath(absU)), "the file is at the UTF-8 absolute path");
        }

        // ---- raw recording, and F2 while recording ----
        wd.mark("raw recording");
        {
            bridge::RecordingCommand record;
            record.action = bridge::RecordingCommandAction::StartFrameRecording;
            record.filePath = "../clip.h5";
            bridge::RecordingCommand stop;
            stop.action = bridge::RecordingCommandAction::StopFrameRecording;
            const auto escape = facade.dispatch(record);
            if (escape.ok) {
                (void)facade.dispatch(stop);   // a wrongly started recording is stopped
                (void)waitFor([&] { return !backendApp.isFrameRecording(); }, std::chrono::seconds(10));
            }
            MIB_EXPECT(!escape.ok && contains(escape.message, "leaves the data directory") && !fs::exists(base / "clip.h5"),
                       "raw recording escaping the root is refused: " + escape.message);
            record.filePath = "";
            const auto empty = facade.dispatch(record);
            MIB_EXPECT(!empty.ok && contains(empty.message, "no output path given"), "raw recording with no path: " + empty.message);
            record.filePath = "clips/clip.h5";
            const auto started = facade.dispatch(record);
            MIB_REQUIRE(started.ok && backendApp.isFrameRecording(), "relative raw recording starts: " + started.message);
            // F2: while raw recording holds the file, the gate and Start point at the recording, not at Review.
            const auto during = coordinator.evaluateReadiness("during.h5");
            const auto* hdf5Gate = during.gate("storage.hdf5");
            MIB_EXPECT(hdf5Gate && hdf5Gate->status == GateStatus::Fail &&
                           hdf5Gate->remediation == "finish or close the current file first" &&
                           statusOf(during, "lifecycle.recording") == GateStatus::Fail,
                       "recording: storage.hdf5 does not send the operator to Review");
            const auto refused = startRun(facade, "during.h5");
            MIB_EXPECT(!refused.ok && refused.experimentStartOutcome == ExperimentStartOutcome::NotReady &&
                           refused.message == "raw recording is active: stop it before starting an experiment",
                       "Start during raw recording says to stop it: " + refused.message);
            MIB_EXPECT(facade.dispatch(stop).ok, "raw recording stops");
            MIB_EXPECT(waitFor([&] { return !backendApp.isFrameRecording(); }, std::chrono::seconds(10)), "recording stopped");
            MIB_EXPECT(fs::exists(R / "clips" / "clip.h5") && !fs::exists(cwd / "clips"),
                       "the raw recording is under the storage root");
        }

        // ---- the review export, and F2 with a file open for review ----
        wd.mark("review export");
        {
            const auto escape = facade.submitReviewExportJson(
                nlohmann::json{{"format", "metrics_csv"}, {"output_root", "../exports"}}.dump());
            MIB_EXPECT(!escape.ok && contains(escape.message, "leaves the data directory") && !fs::exists(base / "exports"),
                       "an export root escaping the root is refused: " + escape.message);

            // A small file with two valid frames to review and export (the short runs above saved none).
            const fs::path reviewSource = R / "review_source.h5";
            {
                backend::services::Hdf5Service writer;
                std::vector<backend::services::ProcessedFrame> frames(2);
                for (size_t i = 0; i < frames.size(); ++i) {
                    frames[i].originalImage = cv::Mat(32, 32, CV_8UC1, cv::Scalar(static_cast<double>(40 + i)));
                    frames[i].processedImage = frames[i].originalImage.clone();
                }
                MIB_REQUIRE(writer.openFile(reviewSource.string()) && writer.appendFrames(frames, {}) && writer.flush(),
                            "write the review source");
                writer.closeFile();
            }
            bridge::RecordingLoadCommand load;
            load.filePath = reviewSource.string();
            MIB_REQUIRE(facade.dispatch(load).ok && backendApp.reviewSourceOpen(), "open a file for review");
            // F2: the gate and Start's refusal say how to close it.
            const auto readiness = coordinator.evaluateReadiness("blocked.h5");
            const auto* hdf5Gate = readiness.gate("storage.hdf5");
            MIB_EXPECT(hdf5Gate && hdf5Gate->status == GateStatus::Fail && hdf5Gate->reason == "an HDF5 file is already open" &&
                           hdf5Gate->remediation == app::kCloseReviewFile,
                       "review: storage.hdf5 says Close the file in Review");
            const auto refused = startRun(facade, "blocked.h5");
            MIB_EXPECT(!refused.ok && refused.experimentStartOutcome == ExperimentStartOutcome::NotReady &&
                           contains(refused.message, app::kCloseReviewFile),
                       "review: Start's refusal says Close the file in Review: " + refused.message);
            MIB_EXPECT(!fs::exists(R / "blocked.h5"), "the refused Start opened nothing");

            bridge::ReviewCommand exportCsv;
            exportCsv.action = bridge::ReviewCommandAction::ExportMetricsCsv;
            exportCsv.outputPath = "exports/metrics.csv";
            const auto started = facade.dispatch(exportCsv);
            MIB_EXPECT(started.ok && started.operationId != 0, "relative metrics export starts: " + started.message);
            if (started.ok) {
                MIB_EXPECT(waitFor([&] { return terminalOf(started.operationId).has_value(); }, std::chrono::seconds(20)),
                           "the export finishes");
                const auto done = terminalOf(started.operationId);
                MIB_EXPECT(done && done->state == bridge::BackendOperationState::Completed,
                           "the export completes: " + (done ? done->message : std::string("-")));
                MIB_EXPECT(fs::exists(R / "exports" / "metrics.csv") && !fs::exists(cwd / "exports"),
                           "the CSV is under the storage root");
            }
            MIB_EXPECT(facade.closeReview().ok && !backendApp.reviewSourceOpen(), "close the review file");
        }

        wd.mark("regenerate and preview save");
        {
            const auto regenerate = facade.submitReviewReanalysisJson(
                nlohmann::json{{"source_path", runFile.string()}, {"output_path", "../regenerated.h5"}}.dump());
            MIB_EXPECT(!regenerate.ok && contains(regenerate.message, "leaves the data directory"),
                       "a Regenerate output escaping the root is refused: " + regenerate.message);
            const auto preview = nlohmann::json::parse(facade.savePreviewBufferJson(
                nlohmann::json{{"action", "save"}, {"first", "0"}, {"last", "0"}, {"output_root", "../previews"}}.dump()));
            MIB_EXPECT(!preview.value("ok", true) && contains(preview.value("error", std::string()), "leaves the data directory"),
                       "a preview save root escaping the root is refused: " + preview.dump());
        }

        MIB_EXPECT(entries(cwd) == 0, "the working directory stayed empty throughout");
        facade.shutdown();
    }
    fs::current_path(previousCwd);
    return mib::test::exitCode();
}
