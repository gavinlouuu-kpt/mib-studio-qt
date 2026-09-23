#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "support/assert.h"
#include "backend/processing/ProcessingService.h"
#include "backend/services/AutofocusService.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
int main() {
    mib::test::Watchdog watchdog;
    mib::test::TempDir temp;
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(temp.path().string()), "initialize");
    using J = nlohmann::json;
    const auto base = (temp / "profiles").string();
    auto call = [&](J q) { return J::parse(facade.profileCommand(base, q.dump())); };
    J create = {
        {"operation", "create"},
        {"name", "one"},
        {"document_json", R"({"image_processing":{"custom":17},"unknown":{"preserve":true}})"},
        {"script", "// keep camera script\n"}};
    auto r = call(create);
    MIB_REQUIRE(r["ok"], r.dump());
    const auto rev = r["profile"]["revision"];
    auto read = call({{"operation", "read"}, {"name", "one"}});
    MIB_EXPECT(read["profile"]["document_json"] == create["document_json"] &&
                   read["profile"]["script"] == create["script"],
               "roundtrip exact bytes");
    MIB_EXPECT(!call(create)["ok"], "never overwrite");
    auto copy =
        call({{"operation", "duplicate"}, {"source", "one"}, {"name", "copy"}, {"baseline", rev}});
    MIB_EXPECT(copy["ok"], copy.dump());
    std::ofstream(std::filesystem::path(base) / "one" / "egrabberConfig.js") << "changed";
    MIB_EXPECT(!call({{"operation", "archive"}, {"name", "one"}, {"baseline", rev}})["ok"],
               "script change invalidates baseline");
    auto renamed = call({{"operation", "rename"},
                         {"name", "copy"},
                         {"destination", "renamed"},
                         {"baseline", copy["profile"]["revision"]}});
    MIB_REQUIRE(renamed["ok"], renamed.dump());
    auto archived = call({{"operation", "archive"},
                          {"name", "renamed"},
                          {"baseline", renamed["profile"]["revision"]}});
    MIB_EXPECT(archived["ok"] &&
                   std::filesystem::exists(archived["destination"].get<std::string>()),
               "archive recoverable");
    for (const auto* bad : {"../escape", "/absolute", ".hidden", "bad/name"}) {
        create["name"] = bad;
        MIB_EXPECT(!call(create)["ok"], "reject path escape");
    }
    create["name"] = "invalid";
    create["document_json"] = "[]";
    MIB_EXPECT(!call(create)["ok"], "root object required");
    // Filesystem fault: target occupied by file. No staging artifact and no deletion.
    std::ofstream(std::filesystem::path(base) / "blocked") << "keep";
    create["name"] = "blocked";
    create["document_json"] = "{}";
    MIB_EXPECT(!call(create)["ok"] &&
                   std::filesystem::is_regular_file(std::filesystem::path(base) / "blocked"),
               "occupied target preserved");
#ifndef _WIN32
    std::filesystem::create_directory_symlink(temp.path(), std::filesystem::path(base) / "link");
    MIB_EXPECT(!call({{"operation", "read"}, {"name", "link"}})["ok"], "symlink rejected");
#endif
    std::vector<std::thread> threads;
    std::vector<J> results(8);
    create["name"] = "concurrent";
    for (size_t i = 0; i < results.size(); ++i)
        threads.emplace_back([&, i] { results[i] = call(create); });
    for (auto& t : threads)
        t.join();
    int saves = 0;
    for (const auto& r : results)
        saves += r["ok"].get<bool>();
    MIB_EXPECT(saves == 1, "serialized concurrent creation");
    // Validate entire candidate before mutating runtime state, not processing-only.
    create["name"] = "runtime";
    create["document_json"] =
        R"({"image_processing":{"area_threshold_min":42},"pixel_to_micron_factor":0.7,"buffer_threshold":55,"autofocus_focus_setpoint":31,"realtime_processing":{"mode":"inline","batch_size":4,"max_queued_frames":40},"camera":{"frame_delivery_mode":"latestFrame"}})";
    const auto runtime = call(create);
    MIB_REQUIRE(runtime["ok"], runtime.dump());
    std::ofstream(std::filesystem::path(base) / "runtime" / "profile.meta.json")
        << R"({"profile_meta_schema_version":1,"processing_contract_version":null,"app_min_version":"","app_max_version":null})";
    const auto withMetadata = call({{"operation", "read"}, {"name", "runtime"}});
    auto applied = call({{"operation", "apply"},
                         {"name", "runtime"},
                         {"baseline", withMetadata["profile"]["revision"]}});
    MIB_REQUIRE(applied["ok"] && applied["applied"], applied.dump());
    MIB_EXPECT(backend.processing().getPixelToMicronFactor() == 0.7 &&
                   backend.processing().getFlushInterval() == 55 &&
                   backend.autofocus().getConfig().focusSetpoint == 31,
               "nonprocessing settings applied");
    create["name"] = "invalid-runtime";
    create["document_json"] =
        R"({"image_processing":{"area_threshold_min":99},"pixel_to_micron_factor":-1})";
    const auto invalid = call(create);
    MIB_REQUIRE(invalid["ok"], invalid.dump());
    auto rejected = call({{"operation", "apply"},
                          {"name", "invalid-runtime"},
                          {"baseline", invalid["profile"]["revision"]}});
    MIB_EXPECT(!rejected["ok"] &&
                   backend.processing().getProcessingConfig().area_threshold_min == 42,
               "later invalid calibration cannot partially apply processing");
    const auto listed = call({{"operation", "list"}});
    MIB_EXPECT(listed["profiles"].size() == 4, "archives and malformed entries excluded");
    backend.shutdown();
    return mib::test::exitCode();
}
