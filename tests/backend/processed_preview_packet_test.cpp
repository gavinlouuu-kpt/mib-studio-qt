#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/playback/FrameStore.h"
#include "backend/processing/ProcessingService.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <thread>
#include <cstdlib>
using namespace std::chrono_literals;
namespace {
nlohmann::json metadata(const std::vector<std::uint8_t>& bytes, size_t& offset) {
    MIB_EXPECT(bytes.size() >= 12 && std::string(bytes.begin(), bytes.begin() + 4) == "MIPO",
               "packet header");
    std::uint32_t count = 0;
    for (int i = 0; i < 4; ++i)
        count |= std::uint32_t(bytes[8 + i]) << (i * 8);
    offset = 12 + count;
    return nlohmann::json::parse(bytes.begin() + 12, bytes.begin() + offset);
}
void setEnv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
} // namespace
int main() {
    mib::test::Watchdog watchdog(40);
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_DISABLED_SERVICES", "yolo,auto_update");
    for (bool batch : {false, true}) {
        mib::test::TempDir dir("processed-preview");
        backend::AppBackend app;
        backend::bridge::BackendFacade facade(app);
        MIB_REQUIRE(facade.initialize(dir.path().string()), "initialize without hardware capture");
        auto store = app.getFrameStore();
        auto& proc = app.processing();
        auto config = proc.getProcessingConfig();
        config.gaussian_blur_size = 1;
        config.bg_subtract_threshold = 100;
        config.empty_frame_pixel_threshold = 1;
        config.auto_background_enabled = false;
        config.enable_border_check = false;
        config.enable_area_range_check = false;
        config.require_single_inner_contour = false;
        proc.setProcessingConfig(config);
        proc.setRealtimeRoi({4, 4, 56, 56});
        proc.setRealtimeProcessingMode(
            batch ? backend::services::ProcessingService::RealtimeProcessingMode::AsyncBatch
                  : backend::services::ProcessingService::RealtimeProcessingMode::Inline);
        facade.setProcessedPreviewEnabled(true);
        proc.startRealtime(store);
        proc.setRealtimeEnabled(true);
        const auto push = [&] {
            const auto index = store->totalWritten();
            cv::Mat image(64, 64, CV_8UC1, cv::Scalar(0));
            cv::circle(image, {32, 32}, 12, cv::Scalar(220), -1);
            image.at<std::uint8_t>(0, 0) = static_cast<std::uint8_t>(index % 10);
            store->pushFrame(image.data, image.total(), 64, 64, 64, 0x01080001, 1000 + index,
                             2000 + index);
        };
        std::vector<std::uint8_t> frozen;
        nlohmann::json first;
        size_t offset = 0;
        for (int i = 0; i < 200; ++i) {
            push();
            std::this_thread::sleep_for(1ms);
            frozen = facade.fetchProcessedPreviewPacket();
            first = metadata(frozen, offset);
            if (first.at("valid").get<bool>()) break;
        }
        MIB_REQUIRE(first.at("valid").get<bool>(), "processed packet arrives");
        const auto index = std::stoull(first.at("frame_index").get<std::string>());
        MIB_EXPECT(frozen[offset] == index % 10, "pixels match same source index");
        MIB_EXPECT(first.at("source_timestamp") == std::to_string(1000 + index),
                   "timestamp matches pixels");
        MIB_EXPECT(first.at("host_timestamp_us") == std::to_string(2000 + index),
                   "host clock stamp survives");
        MIB_EXPECT(first.at("recipe_sha256").get<std::string>().size() == 64,
                   "frozen recipe identified");
        MIB_EXPECT(first.at("roi") == nlohmann::json({4, 4, 56, 56}),
                   "effective ROI accompanies source pixels");
        MIB_EXPECT(frozen.size() == offset + 8192, "tight atomic image+mask payload");
        const auto frozenCopy = frozen;
        const auto priorRecipe = first.at("recipe_sha256");
        config.bg_subtract_threshold = 110;
        proc.setProcessingConfig(config);
        bool changed = false;
        for (int i = 0; i < 200; ++i) {
            push();
            std::this_thread::sleep_for(1ms);
            auto next = facade.fetchProcessedPreviewPacket();
            size_t off = 0;
            auto nextMeta = metadata(next, off);
            if (nextMeta.at("valid").get<bool>() && nextMeta.at("recipe_sha256") != priorRecipe) {
                changed = true;
                break;
            }
        }
        MIB_EXPECT(changed, "new parameters receive a different recipe identity");
        MIB_EXPECT(frozen == frozenCopy, "old packet remains immutable after newer frames/config");
        proc.stopRealtime();
        backend::playback::Frame oldFrame;
        MIB_REQUIRE(store->getLatest(oldFrame), "last stored frame");
        MIB_REQUIRE(store->resize(store->capacity() + 1), "resize preserves frames");
        backend::playback::Frame newFrame;
        MIB_REQUIRE(store->getLatest(newFrame), "retained frame after renumbering");
        MIB_EXPECT(newFrame.storeGeneration != oldFrame.storeGeneration,
                   "resize advances source epoch even with retained pixels");
        proc.startRealtime(store);
        const auto empty = metadata(facade.fetchProcessedPreviewPacket(), offset);
        MIB_EXPECT(!empty.at("valid").get<bool>() ||
                       empty.at("processing_session") != first.at("processing_session"),
                   "new processing session cannot expose old snapshot");
        facade.setProcessedPreviewEnabled(false);
        facade.shutdown();
    }
    return mib::test::exitCode();
}
