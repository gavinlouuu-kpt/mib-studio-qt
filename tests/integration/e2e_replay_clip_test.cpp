// Start-of-run replay clip, end to end with the mock camera (issue #463,
// docs/exec-plans/active/2026-09-30-replay-clip-capture.md).
//
// Drives real experiments through the BackendFacade with a mock camera that
// loops over distinct, ID-stamped frames, and checks the clip each run leaves
// next to its HDF5 file (<stem>.replay-clip/) and the clip outcome recorded in
// the run's provenance (/run_provenance @replay_clip_json):
//   1. frame limit — exactly N frames, lossless, in acquisition order with no
//      dropped or repeated frame, manifest/index/config/background consistent
//      with the frozen run snapshot, memory released, experiment HDF5 intact;
//      the clip's frames/ folder then replays through the mock camera;
//   2. duration limit — capture stops at the host-time bound;
//   3. early stop — the run ends before the window closes: incomplete clip,
//      run_ended reason, experiment unaffected;
//   4. insufficient free space / disabled — clip skipped, experiment starts
//      and finalizes normally;
//   5. rapid start/stop — Start never waits on a clip, busy clips are
//      skipped, every written clip ends with a final manifest;
//   6. byte cap and free-disk reserve hit mid-write — the clip stops there,
//      keeps what was written, is marked incomplete in its manifest and in
//      the run's provenance (2. also checks a reused output path never
//      overwrites an older clip);
//   7. shutdown with a clip in flight — bounded, truthful manifest;
//   8. every default is overridable through MIB_REPLAY_CLIP* variables.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/ExperimentReadiness.h"
#include "backend/playback/FrameStore.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/ReplayClipRecorder.h"

#include "support/assert.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace bridge = backend::bridge;
    namespace app = backend::app;
    namespace rec = backend::recording;
    namespace fs = std::filesystem;
    using nlohmann::json;

    constexpr int kWidth = 512;
    constexpr int kHeight = 96;
    constexpr int kFixtureFrames = 64;

    fs::path makeTempDir()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<unsigned long long> dist;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto path = fs::temp_directory_path() / ("mib_replay_clip_" + std::to_string(dist(gen)));
            std::error_code ec;
            if (fs::create_directories(path, ec))
            {
                return path;
            }
        }
        throw std::runtime_error("failed to create temporary directory");
    }

    void setEnv(const char *name, const char *value)
    {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    // No naked join/wait that can hang CI: print and _Exit(99).
    struct Watchdog
    {
        std::thread thread;
        std::atomic<bool> done{false};
        explicit Watchdog(int seconds)
        {
            thread = std::thread([this, seconds] {
                for (int i = 0; i < seconds * 10; ++i)
                {
                    if (done.load())
                    {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                std::cerr << "watchdog: e2e_replay_clip_test stuck — exiting\n";
                std::_Exit(99);
            });
        }
        ~Watchdog()
        {
            done.store(true);
            if (thread.joinable())
            {
                thread.join();
            }
        }
    };

    bool waitFor(const std::function<bool()> &pred, int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    // Fixture frame `id`: deterministic texture with the ID stamped into the
    // first two pixels, so every clip frame can be traced to its source.
    cv::Mat fixtureFrame(int id)
    {
        cv::Mat m(kHeight, kWidth, CV_8UC1);
        cv::RNG rng(static_cast<uint64_t>(id) * 7919 + 17);
        rng.fill(m, cv::RNG::UNIFORM, 40, 220);
        m.at<uint8_t>(0, 0) = static_cast<uint8_t>(id);
        m.at<uint8_t>(0, 1) = static_cast<uint8_t>(255 - id);
        return m;
    }

    // Returns the fixture ID of an image, or -1 when it is not a fixture.
    int fixtureId(const cv::Mat &m)
    {
        if (m.empty() || m.type() != CV_8UC1 || m.cols != kWidth || m.rows != kHeight)
        {
            return -1;
        }
        const int id = m.at<uint8_t>(0, 0);
        if (id >= kFixtureFrames || m.at<uint8_t>(0, 1) != 255 - id)
        {
            return -1;
        }
        return cv::norm(m, fixtureFrame(id), cv::NORM_INF) == 0 ? id : -1;
    }

    json readJson(const fs::path &path)
    {
        std::ifstream in(path);
        return json::parse(in);
    }

    std::vector<json> readJsonl(const fs::path &path)
    {
        std::vector<json> out;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty())
            {
                out.push_back(json::parse(line));
            }
        }
        return out;
    }

    std::string readText(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bridge::BackendCommandResult startExperiment(bridge::BackendFacade &facade, const std::string &outputPath)
    {
        app::ExperimentReadinessSnapshot readiness;
        bridge::ExperimentCommand start;
        start.action = bridge::ExperimentCommandAction::Start;
        start.outputPath = outputPath;
        if (facade.fetchExperimentReadiness(readiness, outputPath))
        {
            start.readinessGeneration = readiness.generation;
        }
        return facade.dispatch(start);
    }

    bool stopExperiment(bridge::BackendFacade &facade)
    {
        bridge::ExperimentCommand stop;
        stop.action = bridge::ExperimentCommandAction::Stop;
        if (!facade.dispatch(stop).ok)
        {
            return false;
        }
        return waitFor([&] {
            app::ExperimentStatus s;
            return facade.fetchExperimentStatus(s) && s.terminal && s.state == app::ExperimentRunState::Idle;
        }, 20000);
    }

    bool experimentFileLoads(const std::string &path)
    {
        backend::services::Hdf5Service reader;
        const bool ok = reader.loadFile(path);
        reader.closeFile();
        return ok;
    }

    // The clip outcome the coordinator stored in the run's provenance.
    json clipProvenance(const std::string &h5Path)
    {
        backend::services::Hdf5Service reader;
        std::string text;
        const bool ok = reader.loadFile(h5Path) && reader.readReplayClipJson(text);
        reader.closeFile();
        MIB_EXPECT(ok, "run provenance carries replay_clip_json: " + h5Path);
        return ok ? json::parse(text) : json::object();
    }

    // Where a run's clip must be: next to its HDF5 file.
    fs::path clipDirOf(const std::string &h5Path)
    {
        const fs::path out(h5Path);
        return out.parent_path() / (out.stem().string() + ".replay-clip");
    }

    bool memoryStatsReleased(const rec::ReplayClipRecorder &clips)
    {
        return clips.memoryStats().currentBytes == 0;
    }

    void unsetEnv(const char *name)
    {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

    rec::ReplayClipOptions testOptions()
    {
        rec::ReplayClipOptions o;
        o.freeSpaceReserveBytes = 16ULL << 20; // CI temp dirs can be small
        o.maxWriteBytesPerSec = 0;             // throttling is not under test here
        return o;
    }

    // Common clip checks; returns the parsed index.
    std::vector<json> checkClipFiles(const fs::path &dir, const json &manifest, uint64_t startGeneration)
    {
        MIB_EXPECT(manifest.value("schema_version", 0) == 1, "manifest schema version");
        MIB_EXPECT(manifest.value("kind", "") == "mib.replay_clip", "manifest kind");
        MIB_EXPECT(manifest["run"].value("start_generation", 0ULL) == startGeneration,
                   "manifest run generation matches the experiment");
        MIB_EXPECT(manifest["run"].value("simulated", false), "mock run is marked simulated");
        MIB_EXPECT(manifest.value("config_verified", false),
                   "saved config/background hash to the frozen run snapshot: " + manifest["hashes"].dump());

        const json snapshot = readJson(dir / "run_snapshot.json");
        MIB_EXPECT(snapshot.value("start_generation", 0ULL) == startGeneration, "run_snapshot.json generation");
        MIB_EXPECT(fs::exists(dir / "processing_config.txt"), "processing config saved");

        const cv::Mat bg = cv::imread((dir / "background.png").string(), cv::IMREAD_UNCHANGED);
        MIB_EXPECT(!bg.empty() && cv::norm(bg, fixtureFrame(0), cv::NORM_INF) == 0,
                   "background.png is the background the run used, lossless");

        const auto index = readJsonl(dir / "frames.jsonl");
        MIB_EXPECT(index.size() == manifest["frames"].value("window", 0ULL), "index covers the whole window");
        return index;
    }

    // Every written frame is a lossless fixture frame; consecutive write
    // indices carry consecutive fixture IDs (the mock loops in order), so no
    // frame was dropped, repeated or reordered.
    void checkFramesInOrder(const fs::path &dir, const std::vector<json> &index)
    {
        int previousId = -1;
        uint64_t previousWriteIndex = 0;
        uint64_t expectedOffset = 0;
        uint64_t previousHostTs = 0;
        for (const auto &rec : index)
        {
            MIB_EXPECT(rec.value("offset", ~0ULL) == expectedOffset, "index offsets are consecutive");
            ++expectedOffset;
            MIB_REQUIRE(rec.value("status", "") == "written", "frame written: " + rec.dump());
            const cv::Mat img = cv::imread((dir / rec.value("file", "")).string(), cv::IMREAD_UNCHANGED);
            const int id = fixtureId(img);
            MIB_REQUIRE(id >= 0, "clip frame is a pixel-exact fixture frame: " + rec.value("file", ""));
            MIB_EXPECT(rec.value("width", 0) == kWidth && rec.value("height", 0) == kHeight, "geometry recorded");
            MIB_EXPECT(rec.value("pixel_format", 0ULL) == 0x01080001ULL, "Mono8 recorded");
            const uint64_t writeIndex = rec.value("write_index", 0ULL);
            const uint64_t hostTs = rec.value("host_timestamp_us", 0ULL);
            MIB_EXPECT(hostTs != 0, "host timestamp recorded");
            if (previousId >= 0)
            {
                MIB_EXPECT(writeIndex == previousWriteIndex + 1, "write indices are contiguous");
                MIB_EXPECT(id == (previousId + 1) % kFixtureFrames,
                           "frame " + std::to_string(writeIndex) + " follows its predecessor in the source");
                MIB_EXPECT(hostTs >= previousHostTs, "host timestamps are monotonic");
            }
            previousId = id;
            previousWriteIndex = writeIndex;
            previousHostTs = hostTs;
        }
    }
} // namespace

int main()
{
    Watchdog watchdog(170);
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/manifest.json");

    const fs::path dataDir = makeTempDir();
    const fs::path mockDir = dataDir / "mock_frames";
    fs::create_directories(mockDir);
    for (int id = 0; id < kFixtureFrames; ++id)
    {
        std::ostringstream name;
        name << "frame_" << std::setw(3) << std::setfill('0') << id << ".png";
        MIB_REQUIRE(cv::imwrite((mockDir / name.str()).string(), fixtureFrame(id)), "write mock fixture");
    }
    const fs::path clipRoot = dataDir / "replay-clips";

    {
        backend::AppBackend backendApp;
        bridge::BackendFacade facade(backendApp);
        MIB_REQUIRE(facade.initialize(dataDir.string()), "facade initialize");
        rec::ReplayClipRecorder *clips = backendApp.replayClips();
        MIB_REQUIRE(clips != nullptr, "AppBackend owns a replay clip recorder after initialize");
        MIB_REQUIRE(clips->rootDir() == clipRoot, "clips live under <dataDir>/replay-clips");

        // The product rule: first 1000 frames or 1 second, whichever first.
        const rec::ReplayClipOptions defaults;
        MIB_EXPECT(defaults.enabled && defaults.maxFrames == 1000 && defaults.maxDurationUs == 1'000'000,
                   "default capture rule is 1000 frames or 1 s");

        // Mock camera ~1000 fps over the ID-stamped fixture, looping in order.
        bridge::CameraCommand configure;
        configure.action = bridge::CameraCommandAction::ConfigureMockCamera;
        configure.mockFrameDirectory = mockDir.string();
        configure.mockFrameIntervalMs = 1;
        configure.mockLoopFiles = true;
        MIB_REQUIRE(facade.dispatch(configure).ok, "mock camera configure");
        bridge::CameraCommand startCapture;
        startCapture.action = bridge::CameraCommandAction::StartCapture;
        MIB_REQUIRE(facade.dispatch(startCapture).ok, "capture start");

        // A background the clip must carry (fixture frame 0 as Mono8).
        const cv::Mat background = fixtureFrame(0);
        MIB_REQUIRE(facade.setBackgroundImage(kWidth, kHeight, background.data, background.total()).ok,
                    "set background");

        const std::string probe = (dataDir / "probe.h5").string();
        MIB_REQUIRE(waitFor([&] {
            app::ExperimentReadinessSnapshot r;
            return facade.fetchExperimentReadiness(r, probe) && r.ready;
        }, 10000), "readiness with a running mock camera");

        // ---- 1. Frame limit: exactly N frames, complete and lossless.
        {
            auto opts = testOptions();
            opts.maxFrames = 200;
            opts.maxDurationUs = 30'000'000;
            clips->setOptions(opts);

            const std::string out = (dataDir / "run1.h5").string();
            const auto started = startExperiment(facade, out);
            MIB_REQUIRE(started.ok, "run 1 start: " + started.message);
            const auto run = backendApp.experiment().activeRun();
            MIB_REQUIRE(run.has_value(), "run 1 active");

            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(30)), "clip 1 finishes while the run is active");
            const auto st = clips->lastStatus();
            MIB_EXPECT(st.state == rec::ReplayClipState::Complete,
                       std::string("clip 1 state ") + rec::toString(st.state) + " " + st.message);
            MIB_EXPECT(st.startGeneration == run->startGeneration, "clip belongs to run 1");
            MIB_EXPECT(st.endReason == "frame_limit", "clip 1 end reason " + st.endReason);
            MIB_EXPECT(st.framesCopied == 200 && st.framesWritten == 200 && st.gaps == 0,
                       "clip 1 has exactly 200 frames, no gaps");
            MIB_EXPECT(backendApp.experiment().state() == app::ExperimentRunState::Active,
                       "the experiment keeps running while and after the clip is written");

            const auto mem = clips->memoryStats();
            MIB_EXPECT(mem.currentBytes == 0 && mem.peakBytes > 0 && mem.peakBytes <= opts.maxBytes,
                       "clip memory is bounded and released after writing");
            const auto budget = backendApp.memoryBudgetSnapshot();
            bool reported = false;
            for (const auto &o : budget.owners)
            {
                reported = reported || o.name == "recording.replayClip";
            }
            MIB_EXPECT(reported, "clip buffer appears in the host memory budget");

            const fs::path dir = st.clipDir;
            MIB_REQUIRE(fs::is_directory(dir) && dir == clipDirOf(out),
                        "clip 1 sits next to its recording: " + dir.string());
            MIB_EXPECT(!fs::exists(clipRoot), "nothing goes to the fallback root when the run has an output path");
            const json manifest = readJson(dir / "manifest.json");
            MIB_EXPECT(manifest.value("state", "") == "complete", "manifest state complete");
            MIB_EXPECT(manifest.value("contiguous", false), "manifest contiguous");
            MIB_EXPECT(manifest["frames"].value("written", 0) == 200, "manifest written count");
            MIB_EXPECT(manifest["run"].value("output_path", "") == out, "manifest links the experiment file");
            const auto index = checkClipFiles(dir, manifest, run->startGeneration);
            MIB_EXPECT(index.size() == 200, "index has 200 records");
            checkFramesInOrder(dir, index);
            MIB_EXPECT(index.front().value("write_index", 0ULL) == manifest.value("first_write_index", 1ULL),
                       "first index record is the first write index");

            MIB_REQUIRE(stopExperiment(facade), "run 1 finalizes");
            MIB_EXPECT(experimentFileLoads(out), "run 1 HDF5 is readable (the clip never touched it)");
            const json prov = clipProvenance(out);
            MIB_EXPECT(prov.value("state", "") == "complete" && prov.value("final", false),
                       "run 1 provenance: clip complete " + prov.dump());
            MIB_EXPECT(prov.value("clip_dir", "") == "run1.replay-clip", "provenance names the sibling clip");
            MIB_EXPECT(prov.value("frames_written", 0) == 200 && prov.value("start_generation", 0ULL) ==
                           run->startGeneration, "provenance counts and run identity");

            // The clip's frames/ folder is a mock-camera folder: replay it.
            bridge::CameraCommand stopCapture;
            stopCapture.action = bridge::CameraCommandAction::StopCapture;
            MIB_REQUIRE(facade.dispatch(stopCapture).ok, "capture stop for replay");
            bridge::CameraCommand replay = configure;
            replay.mockFrameDirectory = (dir / "frames").string();
            MIB_REQUIRE(facade.dispatch(replay).ok, "configure mock camera on the clip");
            MIB_REQUIRE(facade.dispatch(startCapture).ok, "start replay capture");
            std::set<int> clipIds;
            for (const auto &r : index)
            {
                clipIds.insert(fixtureId(cv::imread((dir / r.value("file", "")).string(), cv::IMREAD_UNCHANGED)));
            }
            bridge::BackendFrame replayed;
            MIB_EXPECT(waitFor([&] { return facade.fetchLatestFrame(replayed) && !replayed.data.empty(); }, 10000),
                       "replayed clip frames reach the FrameStore");
            const cv::Mat replayedImage(static_cast<int>(replayed.height), static_cast<int>(replayed.width),
                                        CV_8UC1, replayed.data.data(), replayed.strideBytes);
            MIB_EXPECT(clipIds.count(fixtureId(replayedImage)) == 1,
                       "a replayed frame is pixel-identical to a clip frame");

            // Back to the fixture stream for the remaining runs.
            MIB_REQUIRE(facade.dispatch(stopCapture).ok, "stop replay capture");
            MIB_REQUIRE(facade.dispatch(configure).ok, "reconfigure fixture mock camera");
            MIB_REQUIRE(facade.dispatch(startCapture).ok, "restart fixture capture");
            MIB_REQUIRE(facade.setBackgroundImage(kWidth, kHeight, background.data, background.total()).ok,
                        "re-set background");
            MIB_REQUIRE(waitFor([&] {
                app::ExperimentReadinessSnapshot r;
                return facade.fetchExperimentReadiness(r, probe) && r.ready;
            }, 10000), "readiness after replay");
        }

        // ---- 2. Duration limit: capture stops at the host-time bound.
        {
            auto opts = testOptions();
            opts.maxFrames = 1'000'000;
            opts.maxDurationUs = 150'000;
            clips->setOptions(opts);
            const std::string out = (dataDir / "run2.h5").string();
            const auto started = startExperiment(facade, out);
            MIB_REQUIRE(started.ok, "run 2 start: " + started.message);
            const auto run = backendApp.experiment().activeRun();
            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(30)), "clip 2 finishes");
            const auto st = clips->lastStatus();
            MIB_EXPECT(st.state == rec::ReplayClipState::Complete,
                       std::string("clip 2 state ") + rec::toString(st.state) + " " + st.message);
            MIB_EXPECT(st.endReason == "duration_limit", "clip 2 end reason " + st.endReason);
            MIB_EXPECT(st.framesWritten > 0, "clip 2 has frames");
            const json manifest = readJson(fs::path(st.clipDir) / "manifest.json");
            const uint64_t span = manifest.value("last_host_timestamp_us", 0ULL) -
                                  manifest.value("first_host_timestamp_us", 0ULL);
            MIB_EXPECT(span < opts.maxDurationUs, "every clip frame is inside the duration window");
            const auto index = checkClipFiles(st.clipDir, manifest, run->startGeneration);
            checkFramesInOrder(st.clipDir, index);
            MIB_REQUIRE(stopExperiment(facade), "run 2 finalizes");
            MIB_EXPECT(experimentFileLoads(out), "run 2 HDF5 is readable");
            MIB_EXPECT(clipProvenance(out).value("end_reason", "") == "duration_limit",
                       "run 2 provenance: duration limit");

            // Same output path again: the HDF5 is replaced, the older clip is
            // never overwritten or deleted.
            const auto olderGeneration = run->startGeneration;
            MIB_REQUIRE(startExperiment(facade, out).ok, "run 2b reuses the output path");
            const auto rerun = backendApp.experiment().activeRun();
            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(30)), "clip 2b finishes");
            MIB_REQUIRE(stopExperiment(facade), "run 2b finalizes");
            const fs::path second = fs::path(clips->lastStatus().clipDir);
            MIB_EXPECT(second == fs::path(clipDirOf(out).string() + "-1"), "second clip gets a suffix: " +
                                                                               second.string());
            MIB_EXPECT(readJson(clipDirOf(out) / "manifest.json")["run"].value("start_generation", 0ULL) ==
                           olderGeneration, "the older clip is untouched");
            MIB_EXPECT(clipProvenance(out).value("clip_dir", "") == "run2.replay-clip-1" &&
                           clipProvenance(out).value("start_generation", 0ULL) == rerun->startGeneration,
                       "the new run's provenance names its own clip");
        }

        // ---- 3. Run ends before the window closes: incomplete, not lost.
        {
            auto opts = testOptions();
            opts.maxFrames = 1'000'000;
            opts.maxDurationUs = 60'000'000;
            clips->setOptions(opts);
            const std::string out = (dataDir / "run3.h5").string();
            const auto started = startExperiment(facade, out);
            MIB_REQUIRE(started.ok, "run 3 start: " + started.message);
            const auto run = backendApp.experiment().activeRun();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            MIB_REQUIRE(stopExperiment(facade), "run 3 finalizes while its clip is capturing");
            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(30)), "clip 3 finishes after the run ended");
            const auto st = clips->lastStatus();
            MIB_EXPECT(st.startGeneration == run->startGeneration, "clip belongs to run 3");
            MIB_EXPECT(st.state == rec::ReplayClipState::Incomplete,
                       std::string("clip 3 state ") + rec::toString(st.state) + " " + st.message);
            MIB_EXPECT(st.endReason == "run_ended", "clip 3 end reason " + st.endReason);
            MIB_EXPECT(st.framesWritten > 0 && st.framesWritten == st.framesCopied, "copied frames are kept");
            const json manifest = readJson(fs::path(st.clipDir) / "manifest.json");
            MIB_EXPECT(manifest.value("state", "") == "incomplete", "manifest says incomplete");
            checkFramesInOrder(st.clipDir, checkClipFiles(st.clipDir, manifest, run->startGeneration));
            MIB_EXPECT(experimentFileLoads(out), "run 3 HDF5 is readable");
            // Finalization records the clip as it is then; still in flight is
            // allowed (final=false, the manifest is authoritative).
            const json prov = clipProvenance(out);
            const std::string provState = prov.value("state", "");
            MIB_EXPECT(prov.value("start_generation", 0ULL) == run->startGeneration &&
                           (provState == "incomplete" || ((provState == "capturing" || provState == "writing") &&
                                                          !prov.value("final", true))),
                       "run 3 provenance: " + prov.dump());
        }

        // ---- 4. No space for a clip, then capture disabled: skipped, the
        // experiment is unaffected, and no clip directory appears.
        {
            auto opts = testOptions();
            opts.freeSpaceReserveBytes = ~0ULL >> 2;
            clips->setOptions(opts);
            const std::string out4 = (dataDir / "run4.h5").string();
            MIB_REQUIRE(startExperiment(facade, out4).ok, "run 4 starts despite no room for a clip");
            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(10)), "clip 4 settles");
            auto st = clips->lastStatus();
            MIB_EXPECT(st.state == rec::ReplayClipState::Skipped &&
                           st.message.find("free space") != std::string::npos,
                       std::string("clip 4 skipped for space: ") + rec::toString(st.state) + " " + st.message);
            MIB_EXPECT(st.endReason == "disk_reserve", "clip 4 end reason " + st.endReason);
            MIB_REQUIRE(stopExperiment(facade), "run 4 finalizes");
            MIB_EXPECT(experimentFileLoads(out4), "run 4 HDF5 is readable");
            const json prov4 = clipProvenance(out4);
            MIB_EXPECT(prov4.value("state", "") == "skipped" && prov4.value("end_reason", "") == "disk_reserve" &&
                           prov4["clip_dir"].is_null(),
                       "run 4 provenance marks the skipped clip: " + prov4.dump());

            opts = testOptions();
            opts.enabled = false;
            clips->setOptions(opts);
            const std::string out5 = (dataDir / "run5.h5").string();
            MIB_REQUIRE(startExperiment(facade, out5).ok, "run 5 starts with capture disabled");
            st = clips->lastStatus();
            MIB_EXPECT(st.state == rec::ReplayClipState::Skipped && st.message == "disabled",
                       std::string("clip 5 disabled: ") + rec::toString(st.state) + " " + st.message);
            MIB_REQUIRE(stopExperiment(facade), "run 5 finalizes");
            MIB_EXPECT(experimentFileLoads(out5), "run 5 HDF5 is readable");
            MIB_EXPECT(clipProvenance(out5).value("message", "") == "disabled", "run 5 provenance: disabled");

            MIB_EXPECT(!fs::exists(clipDirOf(out4)) && !fs::exists(clipDirOf(out5)),
                       "skipped clips leave no directory behind");
        }

        // ---- 5. Rapid start/stop: a new run while the previous clip is
        // still being written is skipped (never blocks the Start), and every
        // clip that was written ends with a final manifest.
        {
            auto opts = testOptions();
            opts.maxFrames = 300;
            opts.maxDurationUs = 60'000'000;
            clips->setOptions(opts);
            for (int i = 0; i < 12; ++i)
            {
                const std::string out = (dataDir / ("stress" + std::to_string(i) + ".h5")).string();
                const auto t0 = std::chrono::steady_clock::now();
                const auto started = startExperiment(facade, out);
                const auto startMs = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - t0).count();
                MIB_REQUIRE(started.ok, "stress run start " + std::to_string(i) + ": " + started.message);
                MIB_EXPECT(startMs < 5000.0, "Start never waits on a clip");
                std::this_thread::sleep_for(std::chrono::milliseconds(i % 3 == 0 ? 0 : 40));
                MIB_REQUIRE(stopExperiment(facade), "stress run " + std::to_string(i) + " finalizes");
                MIB_EXPECT(experimentFileLoads(out), "stress run HDF5 is readable");
            }
            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(30)), "stress clips settle");
            int busySkips = 0;
            for (int i = 0; i < 12; ++i)
            {
                const std::string out = (dataDir / ("stress" + std::to_string(i) + ".h5")).string();
                const json prov = clipProvenance(out);
                const std::string provState = prov.value("state", "");
                MIB_EXPECT(!provState.empty() && provState != "not_armed", "stress run " + std::to_string(i) +
                                                                                " provenance: " + prov.dump());
                if (provState == "skipped")
                {
                    busySkips += prov.value("message", "").rfind("busy", 0) == 0 ? 1 : 0;
                    MIB_EXPECT(!fs::exists(clipDirOf(out)), "a skipped stress run has no clip directory");
                    continue;
                }
                if (!fs::exists(clipDirOf(out)))
                {
                    continue; // no frame arrived before Stop: skipped after finalization
                }
                const std::string state = readJson(clipDirOf(out) / "manifest.json").value("state", "");
                MIB_EXPECT(state == "complete" || state == "incomplete",
                           "stress clip " + std::to_string(i) + " has a final manifest: " + state);
            }
            std::cerr << "stress: " << busySkips << " runs skipped while the previous clip was writing\n";
            const auto mem = clips->memoryStats();
            MIB_EXPECT(mem.currentBytes == 0, "no clip memory retained after the stress loop");
        }

        // ---- 6a. Clip cap: the clip stops at the cap, keeps what fits, and
        // is marked incomplete in the manifest and the run's provenance.
        {
            auto opts = testOptions();
            opts.maxFrames = 1'000'000;
            opts.maxDurationUs = 60'000'000;
            opts.maxBytes = 50ULL * kWidth * kHeight;
            clips->setOptions(opts);
            const std::string out = (dataDir / "cap.h5").string();
            MIB_REQUIRE(startExperiment(facade, out).ok, "cap run start");
            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(30)), "cap clip finishes");
            const auto st = clips->lastStatus();
            MIB_EXPECT(st.state == rec::ReplayClipState::Incomplete && st.endReason == "byte_limit",
                       std::string("cap clip ") + rec::toString(st.state) + " " + st.endReason);
            MIB_EXPECT(st.framesWritten == 50, "exactly the frames that fit under the cap are kept");
            MIB_EXPECT(readJson(clipDirOf(out) / "manifest.json").value("end_reason", "") == "byte_limit",
                       "manifest records the cap");
            MIB_REQUIRE(stopExperiment(facade), "cap run finalizes");
            const json prov = clipProvenance(out);
            MIB_EXPECT(prov.value("state", "") == "incomplete" && prov.value("end_reason", "") == "byte_limit",
                       "cap run provenance: " + prov.dump());
        }

        // ---- 6b. Free space falls below the reserve while writing: stop
        // writing, keep what was written, account for the rest.
        {
            auto opts = testOptions();
            opts.maxFrames = 120;
            opts.maxDurationUs = 60'000'000;
            clips->setOptions(opts);
            std::atomic<int> probes{0};
            clips->setFreeSpaceProbeForTests([&](const fs::path &) -> std::optional<uint64_t> {
                // Preflight + 20 frames see plenty; then the disk "fills".
                return probes.fetch_add(1) <= 20 ? std::optional<uint64_t>(1ULL << 40)
                                                 : std::optional<uint64_t>(1ULL << 20);
            });
            const std::string out = (dataDir / "reserve.h5").string();
            MIB_REQUIRE(startExperiment(facade, out).ok, "reserve run start");
            MIB_REQUIRE(clips->waitIdle(std::chrono::seconds(30)), "reserve clip finishes");
            clips->setFreeSpaceProbeForTests({});
            const auto st = clips->lastStatus();
            MIB_EXPECT(st.state == rec::ReplayClipState::Incomplete && st.endReason == "disk_reserve",
                       std::string("reserve clip ") + rec::toString(st.state) + " " + st.endReason + " " +
                           st.message);
            MIB_EXPECT(st.framesWritten == 20 && st.framesCopied == 120, "writing stopped at the reserve");
            const auto index = readJsonl(clipDirOf(out) / "frames.jsonl");
            size_t written = 0, unwritten = 0;
            for (const auto &r : index)
            {
                written += r.value("status", "") == "written" ? 1 : 0;
                unwritten += r.value("status", "") == "disk_reserve" ? 1 : 0;
            }
            MIB_EXPECT(written == 20 && unwritten == 100, "every copied frame is accounted for in the index");
            MIB_EXPECT(memoryStatsReleased(*clips), "unwritten frames are released");
            MIB_REQUIRE(stopExperiment(facade), "reserve run finalizes");
            MIB_EXPECT(experimentFileLoads(out), "reserve run HDF5 is readable");
            const json prov = clipProvenance(out);
            MIB_EXPECT(prov.value("end_reason", "") == "disk_reserve" && prov.value("frames_written", 0) == 20,
                       "reserve run provenance: " + prov.dump());
        }

        // ---- 7. Shutdown with a clip in flight is bounded and leaves a
        // truthful manifest.
        {
            auto opts = testOptions();
            opts.maxFrames = 1'000'000;
            opts.maxDurationUs = 60'000'000;
            clips->setOptions(opts);
            const std::string out = (dataDir / "run6.h5").string();
            MIB_REQUIRE(startExperiment(facade, out).ok, "run 6 start");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            facade.shutdown();
            backendApp.shutdown();
            const auto st = clips->lastStatus();
            MIB_EXPECT(st.state == rec::ReplayClipState::Incomplete,
                       std::string("clip 6 after shutdown ") + rec::toString(st.state) + " " + st.message);
            MIB_EXPECT(st.endReason == "run_ended" || st.endReason == "shutdown",
                       "clip 6 end reason " + st.endReason);
            MIB_EXPECT(experimentFileLoads(out), "run 6 HDF5 is readable after shutdown");
        }
    }

    // ---- 8. Every default is overridable from the environment; garbage is
    // ignored, not half-applied.
    {
        const rec::ReplayClipOptions defaults;
        MIB_EXPECT(defaults.maxBytes == (512ULL << 20) && defaults.freeSpaceReserveBytes == (512ULL << 20) &&
                       defaults.maxWriteBytesPerSec == (64ULL << 20),
                   "agreed defaults: 512 MB cap, 512 MB reserve, 64 MB/s");
        setEnv("MIB_REPLAY_CLIP", "0");
        setEnv("MIB_REPLAY_CLIP_MAX_FRAMES", "250");
        setEnv("MIB_REPLAY_CLIP_MAX_MS", "400");
        setEnv("MIB_REPLAY_CLIP_MAX_MB", "128");
        setEnv("MIB_REPLAY_CLIP_RESERVE_MB", "2048");
        setEnv("MIB_REPLAY_CLIP_WRITE_MBPS", "0");
        auto o = rec::replayClipOptionsFromEnvironment();
        MIB_EXPECT(!o.enabled && o.maxFrames == 250 && o.maxDurationUs == 400'000 && o.maxBytes == (128ULL << 20) &&
                       o.freeSpaceReserveBytes == (2048ULL << 20) && o.maxWriteBytesPerSec == 0,
                   "environment overrides apply");
        setEnv("MIB_REPLAY_CLIP_MAX_FRAMES", "-5");
        setEnv("MIB_REPLAY_CLIP_MAX_MB", "lots");
        o = rec::replayClipOptionsFromEnvironment();
        MIB_EXPECT(o.maxFrames == defaults.maxFrames && o.maxBytes == defaults.maxBytes,
                   "invalid overrides are ignored");
        for (const char *name : {"MIB_REPLAY_CLIP", "MIB_REPLAY_CLIP_MAX_FRAMES", "MIB_REPLAY_CLIP_MAX_MS",
                                 "MIB_REPLAY_CLIP_MAX_MB", "MIB_REPLAY_CLIP_RESERVE_MB",
                                 "MIB_REPLAY_CLIP_WRITE_MBPS"})
        {
            unsetEnv(name);
        }
        o = rec::replayClipOptionsFromEnvironment();
        MIB_EXPECT(o.enabled && o.maxFrames == 1000 && o.maxDurationUs == 1'000'000, "unset means defaults");
    }

    std::error_code ec;
    fs::remove_all(dataDir, ec);
    return mib::test::exitCode();
}
