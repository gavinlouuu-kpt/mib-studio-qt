#include "backend/review/RunStatisticsFixture.h"

#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

namespace backend::review
{

    namespace
    {
        constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

        bool fail(std::string *error, const std::string &why)
        {
            if (error) *error = why;
            return false;
        }

        // Row i of the valid set (the test computes its expectations from these formulas).
        services::ProcessedFrame row(int i)
        {
            services::ProcessedFrame f;
            f.index = static_cast<std::uint64_t>(i) * 2;
            f.timestampNs = 1000 + 2000ull * static_cast<std::uint64_t>(i); // ticks at 1 MHz
            f.originalImage = cv::Mat(8, 8, CV_8UC1, cv::Scalar(i % 200));
            f.processedImage = cv::Mat::zeros(8, 8, CV_8UC1);
            auto &v = f.validation;
            v.isValid = !(i == 5 || i == 12 || i == 19 || i == 23);
            v.area = i == 2 ? kNaN : 200.0 + 4.0 * i;
            v.deformability = i == 7 ? kNaN : 0.02 + 0.003 * i;
            v.youngsModulus = (i % 7 == 3) ? kNaN : (i % 7 == 6) ? 0.0 : 1.0 + 0.25 * i;
            v.isTargetGroup = v.isValid && i % 3 == 0;
            v.ringRatio = 0.5;
            v.objectCount = 1;
            return f;
        }

        services::ProcessedFrame invalidRow(int i)
        {
            auto f = row(i);
            f.index = static_cast<std::uint64_t>(i) * 2 + 1;
            f.validation.isValid = false;
            f.validation.isTargetGroup = false;
            return f;
        }

        std::string snapshotJson(RunStatisticsFixture variant)
        {
            const bool even = variant == RunStatisticsFixture::V3WithEEven;
            nlohmann::json lut = {{"loaded", true},
                                  {"lut_id", "fixture_lut"},
                                  {"revision", "1"},
                                  {"source", "bundled-fallback"},
                                  {"sha256", even ? std::string(64, 'b') : std::string(64, 'a')},
                                  {"catalog_sha256", nullptr},
                                  {"checksum_status", nullptr}};
            if (variant == RunStatisticsFixture::LutNotLoaded) lut = {{"loaded", false}};
            return nlohmann::json{
                {"schema_version", 3},
                {"run_id", runStatisticsFixtureRunId(variant)},
                {"start_generation", even ? 8 : 7},
                {"pixel_to_micron", 0.25},
                {"profile_id", even ? "fixture-b" : "fixture"},
                {"experiment_id", "0f0e0d0c-0b0a-4908-8706-050403020100"},
                {"run_index", even ? 2 : 1},
                {"sample_ref", "S1"},
                {"method", {{"source", "local"}, {"config_canonical_sha256", std::string(64, 'c')}}},
                {"processing_core", {{"version", "fixture-core-1"}, {"contract_version", 2}}},
                {"emodulus_lut", lut},
            }
                .dump();
        }
    } // namespace

    std::string runStatisticsFixtureRunId(RunStatisticsFixture variant)
    {
        switch (variant)
        {
        case RunStatisticsFixture::V3WithE: return "11111111-1111-4111-8111-111111111111";
        case RunStatisticsFixture::LutNotLoaded: return "22222222-2222-4222-8222-222222222222";
        case RunStatisticsFixture::V3WithEEven: return "33333333-3333-4333-8333-333333333333";
        case RunStatisticsFixture::EmptyValidSet: return "44444444-4444-4444-8444-444444444444";
        case RunStatisticsFixture::Legacy:
        case RunStatisticsFixture::RawRecording: return {};
        }
        return {};
    }

    bool writeRunStatisticsFixture(const std::string &path, RunStatisticsFixture variant, std::string *error)
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path), ec);
            services::Hdf5Service hdf5;
            if (!hdf5.openFile(path)) return fail(error, "cannot create " + path);
            struct Close
            {
                services::Hdf5Service &h;
                ~Close() { h.closeFile(); }
            } close{hdf5};
            if (variant == RunStatisticsFixture::RawRecording)
            {
                if (!hdf5.initializeRecordingDatasets()) return fail(error, "recording datasets");
                std::vector<cv::Mat> images{cv::Mat::zeros(8, 8, CV_8UC1), cv::Mat::zeros(8, 8, CV_8UC1)};
                std::vector<services::Hdf5Service::RecordingFrameMeta> meta{{0, 0, 8, 8}, {1, 100, 8, 8}};
                if (!hdf5.appendRecordingFrames(images, meta) || !hdf5.writeRecordingInfo(10, 20, 2, 0))
                    return fail(error, "recording frames");
                return true;
            }
            std::vector<services::ProcessedFrame> valid, invalid;
            const int validRows = variant == RunStatisticsFixture::EmptyValidSet ? 0
                                  : variant == RunStatisticsFixture::V3WithEEven ? 26
                                                                                 : 25;
            for (int i = 0; i < validRows; ++i) valid.push_back(row(i));
            for (int i = 0; i < 3; ++i) invalid.push_back(invalidRow(i));
            if (variant == RunStatisticsFixture::LutNotLoaded || variant == RunStatisticsFixture::Legacy)
                for (auto &f : valid) f.validation.youngsModulus = 0.0; // no LUT: E stays 0
            if (!hdf5.initializeDatasets() || !hdf5.appendFrames(valid, invalid)) return fail(error, "frames");
            services::ProcessingConfig config;
            processing::ProcessingCoreIdentity core;
            core.version = "fixture-core-1";
            if (!hdf5.writeExperimentInfo(5'000'000'000ull, 6'000'000'000ull, valid.size(), invalid.size(), config,
                                          services::ProcessingService::Roi{}, nullptr, &core))
                return fail(error, "experiment info");
            if (variant == RunStatisticsFixture::Legacy) return true;
            nlohmann::json configDoc = {
                {"image_processing",
                 {{"target_group",
                   {{"enabled", true},
                    {"area_min", 10},
                    {"area_max", 30},
                    {"deformability_min", 0.0},
                    {"deformability_max", 0.2},
                    {"emodulus_enabled", false}}}}}};
            if (!hdf5.writeConfigJson(configDoc.dump())) return fail(error, "config json");
            if (!hdf5.writeRunSnapshotJson(snapshotJson(variant), "{}")) return fail(error, "snapshot");
            recording::RecordingAccountingSnapshot a;
            a.admitted = 30;
            a.processed = 25;
            a.cancelledByPolicy = 5;
            a.persistenceAdmitted = 25;
            a.persistenceCommitted = 25;
            a.policyAllowsDrops = true;
            a = recording::reconcile(a);
            if (variant == RunStatisticsFixture::EmptyValidSet)
            {
                a = recording::RecordingAccountingSnapshot{};
                a.admitted = 3;
                a.scientificallyRejected = 3;
                a.persistenceAdmitted = 3;
                a.persistenceCommitted = 3;
                a = recording::reconcile(a);
            }
            if (!hdf5.writeRunAccounting(a)) return fail(error, "accounting");
            ::camera::common::TimestampDescriptor descriptor;
            descriptor.ticksPerSecond = 1'000'000;
            descriptor.validity = ::camera::common::TimestampValidity::Valid;
            if (!hdf5.writeAcquisitionProvenance(descriptor, services::AcquisitionTelemetrySnapshot{}))
                return fail(error, "acquisition provenance");
            return true;
        }
        catch (const std::exception &e)
        {
            return fail(error, std::string("fixture: ") + e.what());
        }
    }

} // namespace backend::review
