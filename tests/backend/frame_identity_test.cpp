#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/playback/FrameStore.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <atomic>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <thread>
int main() {
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir dir("facade_frame_identity");
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(dir.path().string()), "initialize");
    auto store = backend.getFrameStore();
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint64_t i = store->totalWritten(); i < 100000; ++i) {
            uint8_t pixels[64]; std::fill(std::begin(pixels), std::end(pixels), uint8_t(i % 251));
            store->pushFrame(pixels, 64, 8, 8, 8, 0x01080001, i);
        }
        done = true;
    });
    size_t reads = 0, mismatches = 0;
    do {
        backend::bridge::BackendFrame frame;
        if (facade.fetchLatestFrame(frame)) {
            ++reads;
            if (frame.timestampNs != frame.frameIndex || frame.data.front() != frame.frameIndex % 251) ++mismatches;
        }
    } while (!done.load());
    producer.join();
    MIB_EXPECT(reads > 0, "concurrent reads exercised");
    MIB_EXPECT(mismatches == 0, "frame index, timestamp and pixels belong to the exact same committed frame");
    const auto range = nlohmann::json::parse(facade.fetchPreviewBufferJson());
    MIB_REQUIRE(range["available"] == true, "buffer available");
    const auto latest = store->latestCommittedIndex();
    nlohmann::json request{{"output_root", dir.path().string()}, {"format", "tiff"},
        {"first", std::to_string(latest - 1)}, {"last", std::to_string(latest)}};
    const auto saved = nlohmann::json::parse(facade.savePreviewBufferJson(request.dump()));
    MIB_REQUIRE(saved["ok"] == true, "save stable buffer");
    const std::filesystem::path output(saved["output_path"].get<std::string>());
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(output)) {
        const auto pixels = cv::imread(entry.path().string(), cv::IMREAD_UNCHANGED);
        MIB_REQUIRE(pixels.rows == 8 && pixels.cols == 8, "roundtrip geometry");
        ++files;
    }
    MIB_EXPECT(files == 2, "exact requested range saved");
    const auto second = nlohmann::json::parse(facade.savePreviewBufferJson(request.dump()));
    MIB_EXPECT(second["ok"] == true && second["output_path"] != saved["output_path"], "no overwrite");
    request["first"] = "0";
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(request.dump()))["ok"] == false, "evicted range rejected not clamped");
    request["output_root"] = (dir / "missing").string();
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(request.dump()))["ok"] == false, "missing destination fault");
    facade.shutdown();
    return mib::test::exitCode();
}
