// F-1 (F01/F04/F16): no stale cache acceptance after an in-place rewrite.
//
// Regression: with no fingerprint requested, the membership cache key used
// the size and mtime recorded at open and never re-stat the file. An in-place
// rewrite with the same row counts reused the cached bitset alongside
// statistics read from the new content.
//
// The test writes two experiment files with the same shape (same row counts,
// same schema) and different values, opens the first, analyses it (filling
// the cache), copies the second file's bytes over the first in place (same
// inode, mtime forced forward), and asserts that the session refuses instead
// of returning the cached membership. Reopening evaluates the new content.
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewSession.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using backend::services::Hdf5Service;
using backend::services::ProcessedFrame;
using Json = nlohmann::json;

namespace {
constexpr std::uint64_t kValid = 2000;
constexpr std::uint64_t kInvalid = 500;

// offset 0: every area in [10, 90); offset 1000: every area in [1010, 1090).
void writeFile(const std::string& path, double offset) {
    Hdf5Service hdf5;
    MIB_REQUIRE(hdf5.openFile(path), "create experiment file");
    MIB_REQUIRE(hdf5.initializeDatasets(), "initialize datasets");
    const auto frame = [&](std::uint64_t i, bool valid) {
        ProcessedFrame f;
        f.index = i;
        f.timestampNs = (i + 1) * 1000;
        f.validation.isValid = valid;
        f.validation.objectId = 0;
        f.validation.objectCount = 1;
        f.validation.area = offset + 10.0 + static_cast<double>(i % 80);
        f.validation.deformability = 0.1;
        f.validation.youngsModulus = 1.5;
        return f;
    };
    std::vector<ProcessedFrame> valid, invalid;
    for (std::uint64_t i = 0; i < kValid; ++i) valid.push_back(frame(i, true));
    for (std::uint64_t i = 0; i < kInvalid; ++i) invalid.push_back(frame(kValid + i, false));
    MIB_REQUIRE(hdf5.appendFrames(valid, invalid), "append rows");
    backend::services::ProcessingConfig cfg;
    backend::services::ProcessingService::Roi roi{0, 0, 512, 96};
    MIB_REQUIRE(hdf5.writeExperimentInfo(1000, 2000, kValid, kInvalid, cfg, roi), "experiment info");
    hdf5.closeFile();
}

Json request(const std::string& path) {
    return Json{{"expectedPath", path},
                {"feature", "area"},
                {"minimum", nullptr},
                {"maximum", nullptr},
                {"graph",
                 {{"id", "rewrite"},
                  {"revision", "1"},
                  {"output", "small"},
                  {"nodes", Json::array({Json{{"id", "small"}, {"parent", ""}, {"kind", "interval"},
                                              {"feature", "area"}, {"minimum", 0.0}, {"maximum", 100.0}}})}}}};
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    mib::test::Watchdog wd(60);
    mib::test::TempDir td("f1_rewrite");
    const auto path = (td.path() / "source.h5").string();
    const auto replacement = (td.path() / "replacement.h5").string();
    writeFile(path, 0.0);
    writeFile(replacement, 1000.0);
    MIB_EXPECT(std::filesystem::file_size(path) == std::filesystem::file_size(replacement),
               "the rewrite keeps the file's shape and size");

    backend::review::ReviewSession session;
    MIB_REQUIRE(session.open(path), "open the original file");
    const auto first = Json::parse(session.analyzePopulation(request(path).dump()));
    MIB_REQUIRE(first.at("ok").get<bool>(), first.dump().substr(0, 400));
    MIB_EXPECT(first.at("selected") == std::to_string(kValid + kInvalid) &&
                   first.at("membershipKey").at("cached") == false,
               "original content: every event inside [0, 100]");
    const auto cachedAgain = Json::parse(session.analyzePopulation(request(path).dump()));
    MIB_EXPECT(cachedAgain.at("ok").get<bool>() && cachedAgain.at("membershipKey").at("cached") == true,
               "unchanged file: membership is reused");
    wd.mark("original analysed");

    // In-place rewrite: same inode, same size, different values; mtime forced
    // forward so the change is visible even on coarse-timestamp filesystems.
    const auto openedTime = std::filesystem::last_write_time(path);
    {
        std::ifstream in(replacement, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
        MIB_REQUIRE(out.good(), "open the source for in-place rewrite");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        MIB_REQUIRE(out.good(), "rewrite in place");
    }
    std::filesystem::last_write_time(path, openedTime + std::chrono::seconds(2));

    for (const bool fingerprint : {false, true}) {
        auto changedRequest = request(path);
        if (fingerprint) changedRequest["fingerprint"] = true;
        const auto stale = Json::parse(session.analyzePopulation(changedRequest.dump()));
        MIB_EXPECT(!stale.at("ok").get<bool>(),
                   std::string("rewritten file is refused, never answered from the cache (fingerprint=") +
                       (fingerprint ? "true" : "false") + "): " + stale.dump().substr(0, 300));
        MIB_EXPECT(!stale.contains("membershipKey"), "no cached membership is returned");
        MIB_EXPECT(stale.value("message", std::string()).find("Source changed") != std::string::npos,
                   "refusal names the source change");
    }
    wd.mark("stale refused");

    // Reopen: a new generation evaluates the new content.
    session.close();
    MIB_REQUIRE(session.open(path), "reopen the rewritten file");
    const auto fresh = Json::parse(session.analyzePopulation(request(path).dump()));
    MIB_REQUIRE(fresh.at("ok").get<bool>(), fresh.dump().substr(0, 400));
    MIB_EXPECT(fresh.at("membershipKey").at("cached") == false && fresh.at("selected") == "0" &&
                   fresh.at("excluded") == std::to_string(kValid + kInvalid),
               "reopened content: no event inside [0, 100]");
    return mib::test::exitCode();
}
