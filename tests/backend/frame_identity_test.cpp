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
            store->pushFrame(pixels, 64, 8, 8, 8, 0x01080001, i, 0, 9007199254740993ULL + i);
        }
        done = true;
    });
    size_t reads = 0, mismatches = 0;
    do {
        backend::bridge::BackendFrame frame;
        if (facade.fetchLatestFrame(frame)) {
            ++reads;
            if (frame.captureSession != 9007199254740993ULL + frame.frameIndex || frame.storeGeneration != 1 || frame.timestampNs != frame.frameIndex || frame.data.front() != frame.frameIndex % 251) ++mismatches;
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
    auto timestampRequest=request;
    timestampRequest["range_mode"]="timestamp";
    timestampRequest["first"]=std::to_string(latest);
    timestampRequest["last"]=std::to_string(latest);
    const auto timestampSave=nlohmann::json::parse(facade.savePreviewBufferJson(timestampRequest.dump()));
    MIB_REQUIRE(timestampSave["ok"]==true,"timestamp subset saves");
    size_t timestampFiles=0;
    for(const auto& entry:std::filesystem::directory_iterator(timestampSave["output_path"].get<std::string>())){(void)entry;++timestampFiles;}
    MIB_EXPECT(timestampFiles==1,"timestamp subset exact inclusive bounds");
    auto invalid=request;invalid["first"]="18446744073709551616";
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(invalid.dump()))["ok"]==false,"u64 overflow rejected");
    invalid=request;invalid["generation"]="0";
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(invalid.dump()))["ok"]==false,"stale ring epoch rejected");
    auto background=nlohmann::json{{"action","background"},{"index",std::to_string(latest)},{"generation",range["generation"]}};
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(background.dump()))["ok"]==true,"paused retained frame background");
    const auto bg=backend.processing().getRealtimeBackgroundGray();
    MIB_REQUIRE(!bg.empty(),"background published");
    MIB_EXPECT(bg.at<uint8_t>(0,0)==latest%251,"background exact selected pixels");
    auto filtered=request;filtered["filter_empty"]=true;
    const auto filterResult=nlohmann::json::parse(facade.savePreviewBufferJson(filtered.dump()));
    MIB_REQUIRE(filterResult["ok"]==true,"shared active-kernel empty filter executes");
    size_t filteredFiles=0;
    for(const auto& entry:std::filesystem::directory_iterator(filterResult["output_path"].get<std::string>())){(void)entry;++filteredFiles;}
    MIB_EXPECT(filteredFiles==0,"uniform background frames omitted by active kernel");
    background["index"]="0";
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(background.dump()))["ok"]==false,"evicted background rejected");
    request["first"] = "0";
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(request.dump()))["ok"] == false, "evicted range rejected not clamped");
    request["output_root"] = (dir / "missing").string();
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(request.dump()))["ok"] == false, "missing destination fault");
    nlohmann::json resize{{"action","resize"},{"capacity","1"}};
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(resize.dump()))["ok"]==false,"destructive shrink requires explicit confirmation");
    resize["confirm_clear"]=true;
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(resize.dump()))["ok"]==true,"confirmed bounded resize");
    const auto resized=nlohmann::json::parse(facade.fetchPreviewBufferJson());
    MIB_EXPECT(resized["capacity"]=="1" && resized["available"]==false,"resize clears retained frames as documented");
    resize["capacity"]="1000001";
    MIB_EXPECT(nlohmann::json::parse(facade.savePreviewBufferJson(resize.dump()))["ok"]==false,"unbounded allocation rejected");
    facade.shutdown();
    return mib::test::exitCode();
}
