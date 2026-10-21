#include "backend/review/RunStatistics.h"

#include "backend/processing/ProcessingTypes.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/review/ReviewSession.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <system_error>

namespace backend::review
{

    namespace
    {
        using nlohmann::json;

        std::string decimal(std::uint64_t v) { return std::to_string(v); }

        json optText(const std::optional<std::string> &v) { return v ? json(*v) : json(nullptr); }
        json optNumber(const std::optional<double> &v)
        {
            return v && std::isfinite(*v) ? json(*v) : json(nullptr);
        }
        json optDecimal(const std::optional<std::uint64_t> &v) { return v ? json(decimal(*v)) : json(nullptr); }

        // A string member of a JSON object, non-empty only.
        std::optional<std::string> nonEmptyString(const json &o, const char *key)
        {
            const auto it = o.find(key);
            if (it == o.end() || !it->is_string()) return std::nullopt;
            auto s = it->get<std::string>();
            if (s.empty()) return std::nullopt;
            return s;
        }

        bool needsFinalLoss(recording::RunCompletionState c)
        {
            return c == recording::RunCompletionState::IntentionallyPartial ||
                   c == recording::RunCompletionState::IncompleteLoss || c == recording::RunCompletionState::Failed;
        }

        json identityJson(const RunStatisticsHeader &h)
        {
            const auto &id = h.identity;
            json o;
            o["has_snapshot"] = id.hasSnapshot;
            o["snapshot_unreadable"] = id.snapshotUnreadable;
            o["snapshot_version"] = id.hasSnapshot ? json(id.snapshotVersion) : json(nullptr);
            o["run_id"] = optText(id.runId);
            o["experiment_id"] = optText(id.declared.experimentId);
            o["run_index"] = id.declared.runIndex ? json(*id.declared.runIndex) : json(nullptr);
            o["sample_ref"] = optText(id.declared.sampleRef);
            o["calibration_record_id"] = optText(id.declared.calibrationRecordId);
            if (id.declared.selectedOperator)
                o["operator"] = {{"id", optText(id.declared.selectedOperator->id)},
                                 {"name", optText(id.declared.selectedOperator->name)}};
            else
                o["operator"] = nullptr;
            if (id.annotation)
            {
                const auto &a = *id.annotation;
                json free = json::object();
                for (const auto &[k, v] : a.free) free[k] = v;
                o["annotation"] = {
                    {"sample", optText(a.sample)},
                    {"condition", optText(a.condition)},
                    {"experimental_unit", optText(a.experimentalUnit)},
                    {"concentration", a.concentration ? json{{"value", a.concentration->value},
                                                             {"unit", a.concentration->unit}}
                                                      : json(nullptr)},
                    {"channel", optText(a.channel)},
                    {"medium", optText(a.medium)},
                    {"temperature_c", optNumber(a.temperatureC)},
                    {"notes", optText(a.notes)},
                    {"free", free},
                };
            }
            else
            {
                o["annotation"] = nullptr;
            }
            o["annotation_unreadable"] = id.annotationUnreadable;
            o["profile_id"] = optText(id.profileId);
            o["method_revision_id"] = optText(h.methodRevisionId);
            o["config_canonical_sha256"] = optText(h.configCanonicalSha256);
            o["core_version"] = optText(h.coreVersion);
            o["contract_version"] = h.contractVersion ? json(*h.contractVersion) : json(nullptr);
            o["start_generation"] = optDecimal(h.startGeneration);
            o["start_time_ns"] = optDecimal(h.startTimeNs);
            o["end_time_ns"] = optDecimal(h.endTimeNs);
            return o;
        }

        json accountingJson(const RunStatisticsHeader &h)
        {
            if (!h.hasAccounting) return nullptr;
            const auto &a = h.accounting;
            return {
                {"completion", recording::toString(a.completion)},
                {"completion_reason", a.completionReason},
                {"reconciled", a.reconciled},
                {"read_error", a.readError.empty() ? json(nullptr) : json(a.readError)},
                {"admitted", decimal(a.admitted)},
                {"processed", decimal(a.processed)},
                {"empty", decimal(a.empty)},
                {"scientifically_rejected", decimal(a.scientificallyRejected)},
                {"processing_failed", decimal(a.processingFailed)},
                {"store_loss", decimal(a.storeOverwritten + a.storeNotCommitted + a.storeMalformed)},
                {"cancelled_by_policy", decimal(a.cancelledByPolicy)},
                {"pending_at_stop", decimal(a.pendingAtStop)},
                {"persistence_admitted", decimal(a.persistenceAdmitted)},
                {"persistence_committed", decimal(a.persistenceCommitted)},
                {"persistence_failed", decimal(a.persistenceFailed)},
                {"objects_detected", decimal(a.objectsDetected)},
            };
        }

        json lutJson(const std::optional<recording::ProvenancedEModulusLut> &record)
        {
            if (!record) return nullptr;
            const auto &l = record->lut;
            return {{"loaded", l.loaded},          {"lut_id", optText(l.lutId)},
                    {"revision", optText(l.revision)}, {"source", optText(l.source)},
                    {"sha256", optText(l.sha256)},    {"provenance", record->provenance}};
        }

        json statisticsJson(const RunStatistics &s)
        {
            const auto &h = s.header;
            const bool factor = h.recordedPixelToMicron > 0.0;
            const bool eComputed = s.eStatus == kRunEComputed;
            json o;
            o["schema_version"] = s.schemaVersion;
            o["identity"] = identityJson(h);
            o["counts"] = {{"set_rows", decimal(s.setRows)},
                           {"valid", decimal(s.validRows)},
                           {"finite", decimal(s.finiteRows)},
                           {"excluded", decimal(s.excludedRows)}};
            o["accounting"] = accountingJson(h);
            o["area_um2"] = {{"n", factor ? json(decimal(s.areaN)) : json(nullptr)},
                             {"median", optNumber(s.areaMedianUm2)},
                             {"pixel_to_micron", factor ? json(h.recordedPixelToMicron) : json(nullptr)}};
            o["deformability"] = {{"n", decimal(s.deformabilityN)}, {"median", optNumber(s.deformabilityMedian)}};
            o["emodulus"] = {{"status", s.eStatus},
                             {"n", eComputed ? json(decimal(s.eN)) : json(nullptr)},
                             {"median_kpa", optNumber(s.eMedianKpa)},
                             {"out_of_domain", eComputed ? json(decimal(s.eOutOfDomain)) : json(nullptr)},
                             {"lut", lutJson(h.eModulusLut)}};
            json bounds = nullptr;
            if (s.targetGroupDeclared && *s.targetGroupDeclared && h.config)
            {
                const auto &c = *h.config;
                bounds = {{"area_um2_min", c.target_group_area_min},
                          {"area_um2_max", c.target_group_area_max},
                          {"deformability_min", c.target_group_deformability_min},
                          {"deformability_max", c.target_group_deformability_max},
                          {"emodulus_gated", c.enable_target_group_emodulus},
                          {"emodulus_kpa_min",
                           c.enable_target_group_emodulus ? json(c.target_group_emodulus_min) : json(nullptr)},
                          {"emodulus_kpa_max",
                           c.enable_target_group_emodulus ? json(c.target_group_emodulus_max) : json(nullptr)}};
            }
            const bool declared = s.targetGroupDeclared && *s.targetGroupDeclared;
            o["target_group"] = {
                {"declared", s.targetGroupDeclared ? json(*s.targetGroupDeclared) : json(nullptr)},
                {"n", declared ? json(decimal(s.targetGroupN)) : json(nullptr)},
                {"of", declared ? json(decimal(s.validRows)) : json(nullptr)},
                {"percent", optNumber(s.targetGroupPercent)},
                {"bounds", bounds},
            };
            o["event_rate"] = {{"status", s.eventRateStatus},
                               {"per_s", optNumber(s.eventRatePerS)},
                               {"span_s", optNumber(s.eventRateSpanS)},
                               {"ticks_per_second", h.ticksPerSecond ? json(decimal(h.ticksPerSecond)) : json(nullptr)}};
            o["flags"] = s.flags;
            o["overlay"] = {{"stride", decimal(s.overlayStride)},
                            {"points", decimal(s.overlayDeformability.size())},
                            {"area_px", s.overlayAreaPx},
                            {"area_um2", factor ? json(s.overlayAreaUm2) : json(nullptr)},
                            {"deformability", s.overlayDeformability},
                            {"target_group", s.overlayTargetGroup}};
            return o;
        }

        // "same" | "differs" | "unknown" (either side not recorded).
        template <typename T>
        const char *compare(const std::optional<T> &a, const std::optional<T> &b)
        {
            if (!a || !b) return "unknown";
            return *a == *b ? "same" : "differs";
        }

        json deltaValue(const std::optional<double> &run, const std::optional<double> &ref, const char *withheld)
        {
            if (withheld) return {{"value", nullptr}, {"withheld", withheld}};
            if (!run || !ref) return {{"value", nullptr}, {"withheld", "no_value"}};
            return {{"value", optNumber(*run - *ref)}, {"withheld", nullptr}};
        }

        std::string lowerHex(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        json deltaJson(const RunStatistics &run, const RunStatistics &ref)
        {
            const auto &a = run.header;
            const auto &b = ref.header;
            const char *areaWithheld =
                (a.recordedPixelToMicron > 0.0 && b.recordedPixelToMicron > 0.0) ? nullptr : "factor_not_recorded";
            const char *eWithheld = nullptr;
            if (run.eStatus != kRunEComputed || ref.eStatus != kRunEComputed || !run.eMedianKpa || !ref.eMedianKpa)
                eWithheld = "no_emodulus";
            else
            {
                const auto sa = a.eModulusLut ? a.eModulusLut->lut.sha256 : std::nullopt;
                const auto sb = b.eModulusLut ? b.eModulusLut->lut.sha256 : std::nullopt;
                if (!sa || !sb || sa->empty() || sb->empty()) eWithheld = "lut_unidentified";
                else if (lowerHex(*sa) != lowerHex(*sb)) eWithheld = "lut_differs";
            }
            // The method revision: the central revision when both have one, else the config hash.
            const char *revision = (a.methodRevisionId && b.methodRevisionId)
                                       ? compare(a.methodRevisionId, b.methodRevisionId)
                                       : compare(a.configCanonicalSha256, b.configCanonicalSha256);
            return {
                {"area_um2_median", deltaValue(run.areaMedianUm2, ref.areaMedianUm2, areaWithheld)},
                {"deformability_median", deltaValue(run.deformabilityMedian, ref.deformabilityMedian, nullptr)},
                {"emodulus_median_kpa", deltaValue(run.eMedianKpa, ref.eMedianKpa, eWithheld)},
                {"differs",
                 {{"profile", compare(a.identity.profileId, b.identity.profileId)},
                  {"method_revision", revision},
                  {"core", compare(a.coreVersion, b.coreVersion)},
                  {"contract", compare(a.contractVersion, b.contractVersion)}}},
            };
        }

        json fileJson(const std::optional<RunFileStamp> &stamp)
        {
            if (!stamp) return nullptr;
            // The modification time keys the cache only: std::filesystem's clock has no portable
            // epoch in C++17, so it is not reported.
            return {{"canonical_path", stamp->canonicalPath}, {"size_bytes", decimal(stamp->sizeBytes)}};
        }

        json runJson(const RunStatisticsResult &r, const RunStatistics *reference)
        {
            json o;
            o["path"] = r.path;
            o["status"] = toString(r.status);
            o["message"] = r.message;
            o["computed_at"] = r.computedAt.empty() ? json(nullptr) : json(r.computedAt);
            o["from_cache"] = r.fromCache;
            o["file"] = fileJson(r.stamp);
            const bool computed = r.status == RunStatisticsStatus::Computed && r.stats;
            o["statistics"] = computed ? statisticsJson(*r.stats) : json(nullptr);
            o["delta"] = (computed && reference) ? deltaJson(*r.stats, *reference) : json(nullptr);
            return o;
        }
    } // namespace

    double median(std::vector<double> v)
    {
        const auto n = v.size();
        const auto mid = v.begin() + static_cast<std::ptrdiff_t>(n / 2);
        std::nth_element(v.begin(), mid, v.end());
        const double upper = *mid;
        if (n % 2 == 1) return upper;
        const double lower = *std::max_element(v.begin(), mid);
        return (lower + upper) / 2.0;
    }

    void RunStatisticsRows::add(bool isValid, double areaPx, double deformability, double youngsModulusKpa,
                                bool targetGroup, std::uint64_t timestamp)
    {
        ++setRows_;
        if (!isValid) return;
        ++validRows_;
        areaPx_.push_back(areaPx);
        deformability_.push_back(deformability);
        targetGroup_.push_back(targetGroup ? 1 : 0);
        if (std::isfinite(areaPx) && std::isfinite(deformability)) ++finiteRows_;
        if (targetGroup) ++targetGroupRows_;
        if (!std::isfinite(youngsModulusKpa)) ++eNonFinite_;
        else if (youngsModulusKpa > 0.0) eKpa_.push_back(youngsModulusKpa);
        if (!haveTimestamp_)
        {
            haveTimestamp_ = true;
            timestampMin_ = timestampMax_ = timestamp;
        }
        else
        {
            timestampMin_ = std::min(timestampMin_, timestamp);
            timestampMax_ = std::max(timestampMax_, timestamp);
        }
    }

    RunStatistics computeRunStatistics(RunStatisticsRows rows, const RunStatisticsHeader &header)
    {
        RunStatistics s;
        s.header = header;
        s.setRows = rows.setRows_;
        s.validRows = rows.validRows_;
        s.finiteRows = rows.finiteRows_;
        s.excludedRows = rows.validRows_ - rows.finiteRows_;
        const double f = header.recordedPixelToMicron;
        const bool factor = f > 0.0 && std::isfinite(f);
        // Review's scatter converts each row as area * (f * f) (ReviewSession::scatter).
        const double areaFactor = factor ? f * f : 0.0;

        // Overlay first, from the rows in set order (display only).
        if (s.finiteRows > 0)
        {
            s.overlayStride = (s.finiteRows + kRunStatisticsOverlayMaxPoints - 1) / kRunStatisticsOverlayMaxPoints;
            std::uint64_t k = 0;
            for (std::size_t i = 0; i < rows.areaPx_.size(); ++i)
            {
                const double a = rows.areaPx_[i];
                const double d = rows.deformability_[i];
                if (!std::isfinite(a) || !std::isfinite(d)) continue;
                if (k++ % s.overlayStride != 0) continue;
                s.overlayAreaPx.push_back(a);
                if (factor) s.overlayAreaUm2.push_back(a * areaFactor);
                s.overlayDeformability.push_back(d);
                s.overlayTargetGroup.push_back(rows.targetGroup_[i]);
            }
            rows.targetGroup_ = {};
        }

        // Area µm² at the recorded factor only: compact the area column in place.
        if (factor)
        {
            auto &area = rows.areaPx_;
            std::size_t n = 0;
            for (const double a : area)
            {
                const double um2 = a * areaFactor;
                if (std::isfinite(um2)) area[n++] = um2;
            }
            area.resize(n);
            s.areaN = n;
            if (n > 0) s.areaMedianUm2 = median(std::move(area));
        }
        rows.areaPx_ = {};
        {
            auto &deform = rows.deformability_;
            deform.erase(std::remove_if(deform.begin(), deform.end(), [](double d) { return !std::isfinite(d); }),
                         deform.end());
            s.deformabilityN = deform.size();
            if (!deform.empty()) s.deformabilityMedian = median(std::move(deform));
        }

        // E follows the file's E-LUT record (#764), never the host's.
        if (!header.eModulusLut || !header.eFieldRecorded) s.eStatus = kRunENotRecorded;
        else if (!header.eModulusLut->lut.loaded) s.eStatus = kRunENoLut;
        else
        {
            s.eStatus = kRunEComputed;
            s.eN = rows.eKpa_.size();
            s.eOutOfDomain = rows.eNonFinite_;
            if (!rows.eKpa_.empty()) s.eMedianKpa = median(std::move(rows.eKpa_));
        }

        // Target group: the run's declared group, as recorded (no polygon, Q2 a).
        if (header.config) s.targetGroupDeclared = header.config->enable_target_group;
        if (s.targetGroupDeclared && *s.targetGroupDeclared)
        {
            s.targetGroupN = rows.targetGroupRows_;
            if (s.validRows > 0)
                s.targetGroupPercent =
                    100.0 * static_cast<double>(rows.targetGroupRows_) / static_cast<double>(s.validRows);
        }

        // Event rate on the declared clock: valid rows / (last - first timestamp).
        if (header.ticksPerSecond == 0) s.eventRateStatus = "not_recorded";
        else if (s.validRows < 2 || !rows.haveTimestamp_ || rows.timestampMax_ <= rows.timestampMin_)
            s.eventRateStatus = "too_short";
        else
        {
            const double span = static_cast<double>(rows.timestampMax_ - rows.timestampMin_) /
                                static_cast<double>(header.ticksPerSecond);
            s.eventRateStatus = "computed";
            s.eventRateSpanS = span;
            s.eventRatePerS = static_cast<double>(s.validRows) / span;
        }

        // Descriptive flags only (ruling 6), in a fixed order.
        if (s.validRows < kRunStatisticsLowN) s.flags.emplace_back("low_n");
        if (header.hasAccounting && needsFinalLoss(header.accounting.completion)) s.flags.emplace_back("loss");
        if (!header.identity.hasSnapshot && !header.identity.snapshotUnreadable) s.flags.emplace_back("legacy");
        if (!header.hasAccounting) s.flags.emplace_back("no_accounting");
        if (s.eStatus == kRunEComputed && s.eOutOfDomain > 0) s.flags.emplace_back("e_out_of_domain");
        if (!factor) s.flags.emplace_back("factor_not_recorded");
        // The snapshot is written at Start, the accounting at finalization.
        if (header.identity.hasSnapshot && !header.hasAccounting) s.flags.emplace_back("run_not_finalized");
        return s;
    }

    RunStatisticsHeader readRunStatisticsHeader(const services::Hdf5Service &reader)
    {
        RunStatisticsHeader h;
        h.recordingFile = reader.isRecordingFile();
        h.identity = ReviewSession::readRunIdentity(reader);
        if (h.recordingFile) return h;
        h.recordedPixelToMicron = ReviewSession::recordedPixelToMicron(reader);
        // The E status follows the identity's E-LUT record and its provenance (review ABI 5, #783):
        // the run snapshot v3 record ("snapshot"), else a regenerated file's `@emodulus_lut_json`
        // ("recorded" | "host"); never the host's current LUT.
        if (h.identity.eModulusLut && h.identity.eModulusLutProvenance)
            h.eModulusLut = recording::ProvenancedEModulusLut{*h.identity.eModulusLut, *h.identity.eModulusLutProvenance};
        const auto present = reader.metadataDatasetPresent(true);
        if (present && *present)
        {
            h.metadataPresent = true;
            std::vector<std::string> names;
            if (reader.readMetadataFieldNames(true, names))
                h.eFieldRecorded = std::find(names.begin(), names.end(), "youngsModulus") != names.end();
        }
        else if (present)
        {
            // No valid set at all: no stored row lacks E, so the E-LUT record alone decides.
            h.eFieldRecorded = true;
        }
        h.hasAccounting = reader.readRunAccounting(h.accounting);
        {
            ::camera::common::TimestampDescriptor descriptor;
            services::AcquisitionTelemetrySnapshot telemetry;
            if (reader.readAcquisitionProvenance(descriptor, telemetry) && descriptor.isValid())
                h.ticksPerSecond = descriptor.ticksPerSecond;
        }
        {
            services::ProcessingConfig config;
            // The target group is recorded only in the config document (#758):
            // without one the file does not say whether the run declared one.
            bool groupRecorded = false;
            if (const auto doc = reader.readScalarStringAttribute("/experiment_info", "config_json"))
            {
                const auto root = nlohmann::json::parse(*doc, nullptr, false);
                if (root.is_object())
                    if (const auto p = root.find("image_processing"); p != root.end() && p->is_object())
                        if (const auto t = p->find("target_group"); t != p->end() && t->is_object())
                            groupRecorded = true;
            }
            if (groupRecorded && reader.readRecordedProcessingConfig(config)) h.config = config;
        }
        processing::ProcessingCoreIdentity core;
        if (reader.readProcessingCoreIdentity(core) && !core.version.empty()) h.coreVersion = core.version;
        std::string snapshot;
        if (reader.readRunSnapshotJson(snapshot))
        {
            const auto doc = nlohmann::json::parse(snapshot, nullptr, false);
            if (doc.is_object())
            {
                if (const auto m = doc.find("method"); m != doc.end() && m->is_object())
                {
                    h.methodRevisionId = nonEmptyString(*m, "revision_id");
                    h.configCanonicalSha256 = nonEmptyString(*m, "config_canonical_sha256");
                }
                if (const auto c = doc.find("processing_core"); c != doc.end() && c->is_object())
                {
                    if (!h.coreVersion) h.coreVersion = nonEmptyString(*c, "version");
                    if (const auto v = c->find("contract_version"); v != c->end() && v->is_number_integer())
                        h.contractVersion = v->get<int>();
                }
                if (const auto g = doc.find("start_generation"); g != doc.end() && g->is_number_unsigned())
                    h.startGeneration = g->get<std::uint64_t>();
            }
        }
        if (!h.contractVersion)
        {
            int contract = 0;
            if (reader.readRecordedProcessingContract(contract)) h.contractVersion = contract;
        }
        const auto start = reader.readScalarUintAttribute("/experiment_info", "start_time_ns");
        const auto end = reader.readScalarUintAttribute("/experiment_info", "end_time_ns");
        if (start && *start != 0) h.startTimeNs = start;
        if (end && *end != 0) h.endTimeNs = end;
        return h;
    }

    bool readRunStatisticsRows(const services::Hdf5Service &reader, RunStatisticsRows &out, std::string *error,
                               std::size_t chunkRows)
    {
        out = RunStatisticsRows{};
        const auto present = reader.metadataDatasetPresent(true);
        if (!present)
        {
            if (error) *error = "the run file's valid-object metadata cannot be read";
            return false;
        }
        if (!*present) return true; // no valid objects recorded: an empty set
        const bool ok = reader.readMetadataColumns(
            true, chunkRows == 0 ? services::Hdf5Service::kMetadataColumnChunkRows : chunkRows,
            [&out](const services::Hdf5Service::MetadataColumnRow *rows, std::size_t count) {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto &r = rows[i];
                    out.add(r.isValid != 0, r.area, r.deformability, r.youngsModulus, r.isTargetGroup != 0,
                            r.timestampNs);
                }
                return true;
            });
        if (!ok)
        {
            out = RunStatisticsRows{};
            if (error) *error = "the run file's valid-object metadata cannot be read";
        }
        return ok;
    }

    RunStatisticsRows runStatisticsRowsFromFrames(const std::vector<services::ProcessedFrame> &frames)
    {
        RunStatisticsRows rows;
        for (const auto &f : frames)
        {
            const auto &v = f.validation;
            rows.add(v.isValid, v.area, v.deformability, v.youngsModulus, v.isTargetGroup, f.timestampNs);
        }
        return rows;
    }

    bool stampRunFile(const std::string &path, RunFileStamp &out, std::string &error)
    {
        namespace fs = std::filesystem;
        out = RunFileStamp{};
        if (path.empty())
        {
            error = "No file path";
            return false;
        }
        std::error_code ec;
        const auto p = fs::u8path(path);
        const auto status = fs::status(p, ec);
        if (status.type() == fs::file_type::not_found)
        {
            error = "No such file: " + path;
            return false;
        }
        if (ec)
        {
            error = "Cannot read " + path + ": " + ec.message();
            return false;
        }
        if (status.type() != fs::file_type::regular)
        {
            error = "Not a file: " + path;
            return false;
        }
        const auto canonical = fs::canonical(p, ec);
        if (ec)
        {
            error = "Cannot read " + path + ": " + ec.message();
            return false;
        }
        const auto size = fs::file_size(p, ec);
        if (ec)
        {
            error = "Cannot read " + path + ": " + ec.message();
            return false;
        }
        const auto mtime = fs::last_write_time(p, ec);
        if (ec)
        {
            error = "Cannot read " + path + ": " + ec.message();
            return false;
        }
        out.canonicalPath = canonical.u8string();
        out.sizeBytes = static_cast<std::uint64_t>(size);
        out.mtimeNs = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count());
        return true;
    }

    const char *toString(RunStatisticsStatus status)
    {
        switch (status)
        {
        case RunStatisticsStatus::Computed: return "computed";
        case RunStatisticsStatus::Deferred: return "deferred";
        case RunStatisticsStatus::NotAvailable: return "not_available";
        case RunStatisticsStatus::Error: return "error";
        }
        return "error";
    }

    RunStatisticsResult deferredRunStatistics(const std::string &path, std::string message)
    {
        RunStatisticsResult r;
        r.path = path;
        r.status = RunStatisticsStatus::Deferred;
        r.message = std::move(message);
        return r;
    }

    RunStatisticsResult readRunStatistics(const std::string &path, const std::string &computedAt)
    {
        RunStatisticsResult r;
        r.path = path;
        services::Hdf5Service reader;
        try
        {
            if (!reader.loadFile(path))
            {
                reader.closeFile();
                r.message = "Not an HDF5 file, or it cannot be opened (locked or unreadable): " + path;
                return r;
            }
            auto header = readRunStatisticsHeader(reader);
            if (header.recordingFile)
            {
                reader.closeFile();
                r.status = RunStatisticsStatus::NotAvailable;
                r.message = kRunStatisticsRawMessage;
                return r;
            }
            RunStatisticsRows rows;
            std::string why;
            if (!readRunStatisticsRows(reader, rows, &why))
            {
                reader.closeFile();
                r.message = why;
                return r;
            }
            reader.closeFile();
            r.stats = std::make_shared<const RunStatistics>(computeRunStatistics(std::move(rows), header));
            r.status = RunStatisticsStatus::Computed;
            r.computedAt = computedAt;
        }
        catch (const std::exception &e)
        {
            reader.closeFile();
            r = RunStatisticsResult{};
            r.path = path;
            r.message = "Failed to read " + path + ": " + e.what();
        }
        catch (...)
        {
            reader.closeFile();
            r = RunStatisticsResult{};
            r.path = path;
            r.message = "Failed to read " + path;
        }
        return r;
    }

    RunStatisticsCache::RunStatisticsCache(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

    std::optional<RunStatisticsCache::Entry> RunStatisticsCache::find(const RunFileStamp &stamp)
    {
        std::scoped_lock lock(mutex_);
        const auto it = byPath_.find(stamp.canonicalPath);
        if (it == byPath_.end()) return std::nullopt;
        if (!(it->second->stamp == stamp))
        {
            // The file changed since: its numbers are stale.
            lru_.erase(it->second);
            byPath_.erase(it);
            return std::nullopt;
        }
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->entry;
    }

    void RunStatisticsCache::insert(const RunFileStamp &stamp, Entry entry)
    {
        if (!entry.stats || stamp.canonicalPath.empty()) return;
        std::scoped_lock lock(mutex_);
        if (const auto it = byPath_.find(stamp.canonicalPath); it != byPath_.end())
        {
            lru_.erase(it->second);
            byPath_.erase(it);
        }
        lru_.push_front(Node{stamp, std::move(entry)});
        byPath_[stamp.canonicalPath] = lru_.begin();
        while (lru_.size() > capacity_)
        {
            byPath_.erase(lru_.back().stamp.canonicalPath);
            lru_.pop_back();
        }
    }

    std::size_t RunStatisticsCache::size() const
    {
        std::scoped_lock lock(mutex_);
        return lru_.size();
    }

    void RunStatisticsCache::clear()
    {
        std::scoped_lock lock(mutex_);
        lru_.clear();
        byPath_.clear();
    }

    bool parseRunQuickLookRequest(const std::string &text, RunQuickLookRequest &out, std::string &error)
    {
        out = RunQuickLookRequest{};
        auto fail = [&error](std::string why) {
            error = "invalid quick look request: " + std::move(why);
            return false;
        };
        if (text.size() > 256u * 1024u) return fail("more than 262144 bytes");
        std::vector<std::set<std::string>> keys;
        std::string duplicate;
        const auto callback = [&](int, json::parse_event_t event, json &parsed) {
            using E = json::parse_event_t;
            if (event == E::object_start) keys.emplace_back();
            else if (event == E::object_end && !keys.empty()) keys.pop_back();
            else if (event == E::key && !keys.empty() && !keys.back().insert(parsed.get<std::string>()).second &&
                     duplicate.empty())
                duplicate = parsed.get<std::string>();
            return true;
        };
        json root;
        try
        {
            root = json::parse(text, callback);
        }
        catch (const std::exception &)
        {
            return fail("not valid JSON");
        }
        if (!duplicate.empty()) return fail("duplicate key \"" + duplicate + "\"");
        if (!root.is_object()) return fail("expected a JSON object");
        auto path = [&](const json &v, const std::string &where, std::string &into) {
            if (!v.is_string()) return fail(where + ": expected a string");
            into = v.get<std::string>();
            if (into.empty() || into.size() > kRunQuickLookPathMaxBytes)
                return fail(where + ": a path of 1 to " + std::to_string(kRunQuickLookPathMaxBytes) +
                            " bytes is required");
            return true;
        };
        bool havePaths = false;
        for (auto it = root.begin(); it != root.end(); ++it)
        {
            if (it.key() == "paths")
            {
                const auto &v = it.value();
                if (!v.is_array()) return fail("paths: expected an array of strings");
                if (v.empty() || v.size() > kRunQuickLookMaxPaths)
                    return fail("paths: 1 to " + std::to_string(kRunQuickLookMaxPaths) + " paths are required (got " +
                                std::to_string(v.size()) + ")");
                for (std::size_t i = 0; i < v.size(); ++i)
                {
                    std::string p;
                    if (!path(v[i], "paths[" + std::to_string(i) + "]", p)) return false;
                    out.paths.push_back(std::move(p));
                }
                havePaths = true;
            }
            else if (it.key() == "reference")
            {
                const auto &v = it.value();
                if (v.is_null()) continue;
                if (!v.is_object()) return fail("reference: expected an object or null");
                RunQuickLookReference ref;
                bool havePath = false, haveRunId = false;
                for (auto r = v.begin(); r != v.end(); ++r)
                {
                    if (r.key() == "path")
                    {
                        if (!path(r.value(), "reference.path", ref.path)) return false;
                        havePath = true;
                    }
                    else if (r.key() == "run_id")
                    {
                        haveRunId = true;
                        if (r.value().is_null()) continue;
                        if (!r.value().is_string() || r.value().get<std::string>().empty() ||
                            r.value().get<std::string>().size() > 128)
                            return fail("reference.run_id: expected a string of 1 to 128 bytes or null");
                        ref.runId = r.value().get<std::string>();
                    }
                    else if (r.key() == "start_generation")
                    {
                        if (r.value().is_null()) continue;
                        const auto ok = r.value().is_string() && !r.value().get<std::string>().empty() &&
                                        r.value().get<std::string>().size() <= 20 &&
                                        std::all_of(r.value().get_ref<const std::string &>().begin(),
                                                    r.value().get_ref<const std::string &>().end(),
                                                    [](char c) { return c >= '0' && c <= '9'; });
                        if (!ok) return fail("reference.start_generation: expected a decimal string or null");
                        ref.startGeneration = r.value().get<std::string>();
                    }
                    else
                    {
                        return fail("reference: unknown key \"" + r.key() + "\"");
                    }
                }
                if (!havePath) return fail("reference.path: required");
                if (!haveRunId) return fail("reference.run_id: required (null for a run that records none)");
                out.reference = std::move(ref);
            }
            else
            {
                return fail("unknown key \"" + it.key() + "\"");
            }
        }
        if (!havePaths) return fail("paths: required");
        return true;
    }

    std::string runQuickLookRefusedJson(const std::string &message)
    {
        return json{{"ok", false},
                    {"message", message},
                    {"schema_version", kRunStatisticsSchemaVersion},
                    {"runs", json::array()},
                    {"reference", nullptr}}
            .dump(-1, ' ', false, json::error_handler_t::replace);
    }

    std::string runStatisticsJson(const RunStatistics &stats)
    {
        return statisticsJson(stats).dump(-1, ' ', false, json::error_handler_t::replace);
    }

    std::string runQuickLookReplyJson(const std::vector<RunStatisticsResult> &runs,
                                      const std::optional<RunQuickLookReference> &referenceRequest,
                                      const std::optional<RunStatisticsResult> &reference)
    {
        json reply;
        reply["ok"] = true;
        reply["schema_version"] = kRunStatisticsSchemaVersion;
        const RunStatistics *ref = nullptr;
        if (referenceRequest)
        {
            json block;
            block["requested"] = {{"path", referenceRequest->path},
                                  {"run_id", optText(referenceRequest->runId)},
                                  {"start_generation", optText(referenceRequest->startGeneration)}};
            bool confirmed = false;
            std::string message;
            if (!reference)
            {
                message = "the reference was not read";
            }
            else if (reference->status != RunStatisticsStatus::Computed || !reference->stats)
            {
                message = "the reference is " + std::string(toString(reference->status)) +
                          (reference->message.empty() ? std::string() : ": " + reference->message);
            }
            else
            {
                const auto &fileRunId = reference->stats->header.identity.runId;
                if (fileRunId == referenceRequest->runId)
                {
                    confirmed = true;
                    ref = reference->stats.get();
                }
                else
                {
                    message = "run_id mismatch: the file records " +
                              (fileRunId ? "run " + *fileRunId : std::string("no run_id")) + ", the reference names " +
                              (referenceRequest->runId ? "run " + *referenceRequest->runId : std::string("none"));
                }
            }
            block["confirmed"] = confirmed;
            block["message"] = message;
            block["run"] = reference ? runJson(*reference, nullptr) : json(nullptr);
            reply["reference"] = block;
        }
        else
        {
            reply["reference"] = nullptr;
        }
        json list = json::array();
        std::size_t computed = 0, deferred = 0;
        for (const auto &r : runs)
        {
            if (r.status == RunStatisticsStatus::Computed) ++computed;
            if (r.status == RunStatisticsStatus::Deferred) ++deferred;
            list.push_back(runJson(r, ref));
        }
        reply["runs"] = list;
        reply["message"] = std::to_string(computed) + " of " + std::to_string(runs.size()) + " run(s) computed" +
                           (deferred ? ", " + std::to_string(deferred) + " deferred (" + kRunStatisticsDeferredMessage + ")"
                                     : std::string());
        return reply.dump(-1, ' ', false, json::error_handler_t::replace);
    }

} // namespace backend::review
