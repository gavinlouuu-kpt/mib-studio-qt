#include "backend/app/CalibrationRecord.h"

#include "backend/profiles/InstrumentIdentity.h"
#include "backend/recording/Hdf5Service.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace backend::app {

namespace {

using nlohmann::json;
constexpr double kPi = 3.14159265358979323846;

bool fail(std::string* why, const std::string& text)
{
    if (why) *why = text;
    return false;
}

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

// A number member, finite and in [lo, hi] (exclusive lower bound when `lowOpen`).
bool readNumber(const json& o, const char* key, double lo, double hi, bool lowOpen, double& out, std::string* why,
                const std::string& prefix)
{
    const auto& v = o.at(key);
    if (!v.is_number()) return fail(why, prefix + key + ": expected a number");
    const double d = v.get<double>();
    if (!std::isfinite(d) || (lowOpen ? d <= lo : d < lo) || d > hi) {
        std::ostringstream m;
        m << prefix << key << ": " << d << " is out of range (" << (lowOpen ? "> " : ">= ") << lo << ", <= " << hi
          << ")";
        return fail(why, m.str());
    }
    out = d;
    return true;
}

json optNumber(const std::optional<double>& v)
{
    if (v && std::isfinite(*v)) return *v;
    return nullptr;
}

std::optional<double> getOptNumber(const json& o, const char* key)
{
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number()) return std::nullopt;
    const double d = it->get<double>();
    if (!std::isfinite(d)) return std::nullopt;
    return d;
}

std::string getString(const json& o, const char* key)
{
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

uint64_t getU64(const json& o, const char* key)
{
    const auto it = o.find(key);
    return it != o.end() && it->is_number_unsigned() ? it->get<uint64_t>()
           : it != o.end() && it->is_number_integer() && it->get<int64_t>() >= 0 ? static_cast<uint64_t>(it->get<int64_t>())
                                                                                  : 0;
}

std::string fmt(double v, int precision = 6)
{
    std::ostringstream o;
    o.precision(precision);
    o << v;
    return o.str();
}

} // namespace

const char* toString(MethodPurpose purpose)
{
    return purpose == MethodPurpose::Calibration ? "calibration" : "measurement";
}

const char* toString(CalibrationPolicy policy)
{
    switch (policy) {
    case CalibrationPolicy::None: return "none";
    case CalibrationPolicy::Warn: return "warn";
    case CalibrationPolicy::Required: return "required";
    }
    return "none";
}

const char* toString(CalibrationStatus status)
{
    switch (status) {
    case CalibrationStatus::Proposed: return "proposed";
    case CalibrationStatus::Applied: return "applied";
    case CalibrationStatus::Superseded: return "superseded";
    case CalibrationStatus::Invalidated: return "invalidated";
    case CalibrationStatus::Insufficient: return "insufficient";
    }
    return "unknown";
}

std::optional<CalibrationStatus> calibrationStatusFromString(const std::string& text)
{
    for (auto s : {CalibrationStatus::Proposed, CalibrationStatus::Applied, CalibrationStatus::Superseded,
                   CalibrationStatus::Invalidated, CalibrationStatus::Insufficient}) {
        if (text == toString(s)) return s;
    }
    return std::nullopt;
}

bool parseCalibrationMethodSpec(const std::string& configJson, CalibrationMethodSpec& out, std::string* why)
{
    out = CalibrationMethodSpec{};
    if (configJson.empty()) return true;
    json root;
    try {
        root = json::parse(configJson);
    } catch (const std::exception&) {
        return fail(why, "config.json is not valid JSON");
    }
    if (!root.is_object()) return fail(why, "config.json root must be an object");
    CalibrationMethodSpec spec;
    if (const auto it = root.find("purpose"); it != root.end() && !it->is_null()) {
        if (!it->is_string()) return fail(why, "purpose: expected \"measurement\" or \"calibration\"");
        const auto text = it->get<std::string>();
        if (text == "measurement") spec.purpose = MethodPurpose::Measurement;
        else if (text == "calibration") spec.purpose = MethodPurpose::Calibration;
        else return fail(why, "purpose: \"" + text + "\" is not \"measurement\" or \"calibration\"");
    }
    if (const auto it = root.find("calibration_policy"); it != root.end() && !it->is_null()) {
        if (!it->is_string()) return fail(why, "calibration_policy: expected \"none\", \"warn\" or \"required\"");
        const auto text = it->get<std::string>();
        if (text == "none") spec.policy = CalibrationPolicy::None;
        else if (text == "warn") spec.policy = CalibrationPolicy::Warn;
        else if (text == "required") spec.policy = CalibrationPolicy::Required;
        else return fail(why, "calibration_policy: \"" + text + "\" is not \"none\", \"warn\" or \"required\"");
    }
    if (const auto it = root.find("calibration_max_age_hours"); it != root.end() && !it->is_null()) {
        if (!readNumber(root, "calibration_max_age_hours", 0.0, 87600.0, true, spec.maxAgeHours, why, ""))
            return false;
    }
    const auto ref = root.find("calibration_reference");
    const bool hasRef = ref != root.end() && !ref->is_null();
    if (spec.purpose == MethodPurpose::Measurement) {
        if (hasRef)
            return fail(why, "calibration_reference: only a method with purpose \"calibration\" declares a reference");
    } else {
        if (!hasRef)
            return fail(why, "calibration_reference: a calibration method must declare its reference "
                             "(nominal_diameter_um and diameter_tolerance_um)");
        if (!ref->is_object()) return fail(why, "calibration_reference: expected an object");
        const std::string p = "calibration_reference.";
        for (auto m = ref->begin(); m != ref->end(); ++m) {
            const auto& k = m.key();
            if (k != "nominal_diameter_um" && k != "diameter_tolerance_um" && k != "nominal_e_kpa" &&
                k != "max_cv_percent")
                return fail(why, p + k + ": unknown member");
        }
        CalibrationReference r;
        if (!ref->contains("nominal_diameter_um")) return fail(why, p + "nominal_diameter_um: required");
        if (!readNumber(*ref, "nominal_diameter_um", 0.0, 1000.0, true, r.nominalDiameterUm, why, p)) return false;
        if (!ref->contains("diameter_tolerance_um")) return fail(why, p + "diameter_tolerance_um: required");
        if (!readNumber(*ref, "diameter_tolerance_um", 0.0, r.nominalDiameterUm, false, r.diameterToleranceUm, why, p))
            return false;
        if (ref->contains("nominal_e_kpa") && !ref->at("nominal_e_kpa").is_null()) {
            double e = 0.0;
            if (!readNumber(*ref, "nominal_e_kpa", 0.0, 1e6, true, e, why, p)) return false;
            r.nominalEKpa = e;
        }
        if (ref->contains("max_cv_percent") && !ref->at("max_cv_percent").is_null()) {
            if (!readNumber(*ref, "max_cv_percent", 0.0, 100.0, true, r.maxCvPercent, why, p)) return false;
        }
        spec.reference = r;
    }
    out = spec;
    return true;
}

std::string describeIdentityChange(const CalibrationIdentity& from, const CalibrationIdentity& to)
{
    std::string out;
    auto add = [&](const char* name, const std::string& a, const std::string& b) {
        if (a == b) return;
        if (!out.empty()) out += "; ";
        out += std::string(name) + " " + (a.empty() ? "unknown" : a) + " -> " + (b.empty() ? "unknown" : b);
    };
    add("camera", from.camera, to.camera);
    add("ROI", from.roi, to.roi);
    add("chip", from.chip, to.chip);
    add("objective", from.objective, to.objective);
    return out;
}

CalibrationIdentity calibrationIdentityOf(const RunConfigurationSnapshot& c)
{
    CalibrationIdentity id;
    const auto& source = c.camera.effective;
    if (!source.empty() && source != "unknown" && source != "none")
        id.camera = c.camera.label.empty() ? source : source + ":" + c.camera.label;
    if (!c.roiPending) {
        id.roi = (c.roiW > 0 && c.roiH > 0) ? std::to_string(c.roiX) + "," + std::to_string(c.roiY) + "," +
                                                  std::to_string(c.roiW) + "," + std::to_string(c.roiH)
                                            : std::string("full");
    }
    return id;
}

bool readCalibrationEvidence(const std::string& path, CalibrationEvidence& out, std::string* why)
{
    out = CalibrationEvidence{};
    // A reader of its own: the run's writer has closed the file (see the coordinator's finalization).
    services::Hdf5Service reader;
    if (!reader.loadFile(path)) return fail(why, "the run file cannot be opened: " + path);
    struct Close {
        services::Hdf5Service& r;
        ~Close() { r.closeFile(); }
    } close{reader};
    const auto present = reader.metadataDatasetPresent(true);
    if (!present) return fail(why, "the run file's valid-object metadata cannot be read");
    if (!*present) return true; // no valid objects recorded
    std::vector<services::ProcessedFrame> rows;
    if (!reader.readValidMetadata(rows)) return fail(why, "the run file's valid-object metadata cannot be read");
    std::vector<std::string> names;
    if (reader.readMetadataFieldNames(true, names)) {
        out.focusFieldRecorded = std::find(names.begin(), names.end(), "laplacianVariance") != names.end();
        out.eFieldRecorded = std::find(names.begin(), names.end(), "youngsModulus") != names.end();
    }
    ::camera::common::TimestampDescriptor descriptor;
    services::AcquisitionTelemetrySnapshot telemetry;
    if (reader.readAcquisitionProvenance(descriptor, telemetry) && descriptor.isValid())
        out.ticksPerSecond = descriptor.ticksPerSecond;
    out.objects.reserve(rows.size());
    for (const auto& row : rows) {
        if (!row.validation.isValid) continue;
        CalibrationSample s;
        s.frameIndex = row.index;
        s.timestamp = row.timestampNs;
        s.areaPx = row.validation.area;
        s.deformation = row.validation.deformability;
        s.laplacianVariance = row.validation.laplacianVariance;
        s.youngsModulusKpa = row.validation.youngsModulus;
        out.objects.push_back(s);
    }
    return true;
}

CalibrationDerivation deriveCalibration(const CalibrationEvidence& evidence, const CalibrationReference& reference,
                                        double runPixelToMicron)
{
    CalibrationDerivation d;
    auto& r = d.results;
    std::vector<const CalibrationSample*> counted;
    std::vector<double> diameters;
    counted.reserve(evidence.objects.size());
    diameters.reserve(evidence.objects.size());
    for (const auto& o : evidence.objects) {
        if (!std::isfinite(o.areaPx) || o.areaPx <= 0.0) {
            ++r.excludedObjects;
            continue;
        }
        counted.push_back(&o);
        diameters.push_back(2.0 * std::sqrt(o.areaPx / kPi));
    }
    const auto n = diameters.size();
    r.validObjects = n;
    if (runPixelToMicron > 0.0 && std::isfinite(runPixelToMicron)) r.runPixelToMicron = runPixelToMicron;

    if (n > 0) {
        const double med = median(diameters);
        r.medianDiameterPx = med;
        std::vector<double> dev;
        dev.reserve(n);
        for (double x : diameters) dev.push_back(std::fabs(x - med));
        r.madDiameterPx = median(dev);
        if (n >= 2) {
            double mean = 0.0;
            for (double x : diameters) mean += x;
            mean /= static_cast<double>(n);
            double ss = 0.0;
            for (double x : diameters) ss += (x - mean) * (x - mean);
            const double sd = std::sqrt(ss / static_cast<double>(n - 1));
            if (mean > 0.0) r.cvDiameterPercent = sd / mean * 100.0;
        }
        if (med > 0.0) r.proposedPixelToMicron = reference.nominalDiameterUm / med;
        if (r.runPixelToMicron) {
            r.measuredDiameterUmAtRunFactor = med * *r.runPixelToMicron;
            r.runFactorWithinTolerance =
                std::fabs(*r.measuredDiameterUmAtRunFactor - reference.nominalDiameterUm) <= reference.diameterToleranceUm;
        }
        std::vector<double> deform;
        for (const auto* o : counted)
            if (std::isfinite(o->deformation)) deform.push_back(o->deformation);
        if (!deform.empty()) r.deformationMedian = median(deform);
    }

    // Event-rate stability: valid objects per fixed window over the file's own timestamp clock.
    if (evidence.ticksPerSecond == 0) {
        r.eventRateStatus = kCalibrationNotRecorded;
    } else if (!counted.empty()) {
        std::vector<const CalibrationSample*> byFrame = counted;
        std::stable_sort(byFrame.begin(), byFrame.end(),
                         [](const auto* a, const auto* b) { return a->frameIndex < b->frameIndex; });
        bool monotonic = true;
        for (std::size_t i = 1; i < byFrame.size(); ++i)
            if (byFrame[i]->timestamp < byFrame[i - 1]->timestamp) monotonic = false;
        if (!monotonic) {
            r.eventRateStatus = "not monotonic";
        } else {
            const uint64_t t0 = byFrame.front()->timestamp;
            const double tps = static_cast<double>(evidence.ticksPerSecond);
            const double span = static_cast<double>(byFrame.back()->timestamp - t0) / tps;
            const auto windows = static_cast<std::size_t>(std::floor(span / kCalibrationRateWindowSeconds));
            if (windows < kCalibrationRateMinWindows) {
                r.eventRateStatus = "too short";
                r.eventRateWindows = windows;
            } else {
                std::vector<double> counts(windows, 0.0);
                for (const auto* o : byFrame) {
                    const double t = static_cast<double>(o->timestamp - t0) / tps;
                    const auto w = static_cast<std::size_t>(std::floor(t / kCalibrationRateWindowSeconds));
                    if (w < windows) counts[w] += 1.0;
                }
                double mean = 0.0;
                for (double c : counts) mean += c;
                mean /= static_cast<double>(windows);
                double ss = 0.0;
                for (double c : counts) ss += (c - mean) * (c - mean);
                const double sd = std::sqrt(ss / static_cast<double>(windows - 1));
                r.eventRateStatus = "computed";
                r.eventRateWindows = windows;
                r.eventRateMeanPerS = mean / kCalibrationRateWindowSeconds;
                if (mean > 0.0) r.eventRateCvPercent = sd / mean * 100.0;
            }
        }
    } else {
        r.eventRateStatus = "too short";
    }

    // Focus: only a metric the file records (Contract 2/3 Laplacian variance); never fabricated.
    if (evidence.focusFieldRecorded) {
        std::vector<double> focus;
        for (const auto* o : counted)
            if (std::isfinite(o->laplacianVariance)) focus.push_back(o->laplacianVariance);
        if (!focus.empty()) {
            r.focusStatus = "recorded";
            r.focusLaplacianMedian = median(focus);
            r.focusObjects = focus.size();
        }
    }

    // E against its nominal: only where the method declares one and the file records E.
    if (reference.nominalEKpa) {
        r.eNominalKpa = reference.nominalEKpa;
        std::vector<double> e;
        if (evidence.eFieldRecorded)
            for (const auto* o : counted)
                if (std::isfinite(o->youngsModulusKpa) && o->youngsModulusKpa > 0.0) e.push_back(o->youngsModulusKpa);
        if (e.empty()) {
            r.eStatus = kCalibrationNotRecorded;
        } else {
            r.eStatus = "compared";
            r.eMedianKpa = median(e);
            r.eObjects = e.size();
            r.eRatio = *r.eMedianKpa / *reference.nominalEKpa;
        }
    }

    CalibrationEvidenceGate count{"min_valid_objects", n >= kCalibrationMinValidObjects, static_cast<double>(n),
                                  static_cast<double>(kCalibrationMinValidObjects), {}};
    if (!count.pass)
        count.reason = std::to_string(n) + " valid objects; at least " + std::to_string(kCalibrationMinValidObjects) +
                       " are needed";
    d.gates.push_back(count);
    CalibrationEvidenceGate cv{"max_cv_percent", false, r.cvDiameterPercent.value_or(0.0), reference.maxCvPercent, {}};
    if (!r.cvDiameterPercent) {
        cv.reason = "the diameter CV cannot be computed from " + std::to_string(n) + " objects";
    } else if (*r.cvDiameterPercent > reference.maxCvPercent) {
        cv.reason = "diameter CV " + fmt(*r.cvDiameterPercent, 4) + " % is above the method's limit " +
                    fmt(reference.maxCvPercent, 4) + " %";
    } else {
        cv.pass = true;
    }
    d.gates.push_back(cv);
    return d;
}

std::string utcIsoFromUnixMs(uint64_t unixMs)
{
    const std::time_t t = static_cast<std::time_t>(unixMs / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return out;
}

std::string newCalibrationRecordId(uint64_t unixMs)
{
    const std::time_t t = static_cast<std::time_t>(unixMs / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm);
    return std::string("cal-") + stamp + "-" + profiles::generateUuidV4().substr(0, 8);
}

bool sameCalibrationFactor(double a, double b)
{
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    const double scale = std::max({1e-300, std::fabs(a), std::fabs(b)});
    return std::fabs(a - b) <= kCalibrationFactorRelativeTolerance * scale;
}

std::string calibrationRecordToJson(const CalibrationRecord& rec)
{
    const auto& res = rec.derivation.results;
    json gates = json::array();
    for (const auto& g : rec.derivation.gates)
        gates.push_back({{"id", g.id}, {"pass", g.pass}, {"value", std::isfinite(g.value) ? json(g.value) : json(nullptr)},
                         {"limit", g.limit}, {"reason", g.reason.empty() ? json(nullptr) : json(g.reason)}});
    json j = {
        {"schema_version", rec.schemaVersion},
        {"record_id", rec.recordId},
        {"version", rec.version},
        {"sequence", rec.sequence},
        {"status", toString(rec.status)},
        {"status_reason", rec.statusReason},
        {"status_at_unix_ms", rec.statusAtUnixMs},
        {"status_at_utc", utcIsoFromUnixMs(rec.statusAtUnixMs)},
        {"created_at_unix_ms", rec.createdAtUnixMs},
        {"created_at_utc", utcIsoFromUnixMs(rec.createdAtUnixMs)},
        {"applicable", rec.applicable()},
        {"source",
         {{"run_id", rec.runId.empty() ? json(nullptr) : json(rec.runId)},
          {"file", rec.file},
          {"file_sha256", rec.fileSha256.empty() ? json(nullptr) : json(rec.fileSha256)},
          {"start_generation", rec.startGeneration},
          {"background_sha256", rec.backgroundSha256.empty() ? json(nullptr) : json(rec.backgroundSha256)}}},
        {"method",
         {{"source", rec.methodSource},
          {"method_id", rec.methodId.empty() ? json(nullptr) : json(rec.methodId)},
          {"revision_id", rec.revisionId.empty() ? json(nullptr) : json(rec.revisionId)},
          {"revision_number", rec.revisionNumber},
          {"display_name", rec.methodDisplayName.empty() ? json(nullptr) : json(rec.methodDisplayName)},
          {"config_sha256", rec.configSha256.empty() ? json(nullptr) : json(rec.configSha256)}}},
        {"inputs",
         {{"nominal_diameter_um", rec.reference.nominalDiameterUm},
          {"diameter_tolerance_um", rec.reference.diameterToleranceUm},
          {"nominal_e_kpa", optNumber(rec.reference.nominalEKpa)},
          {"max_cv_percent", rec.reference.maxCvPercent},
          {"min_valid_objects", rec.minValidObjects},
          {"rate_window_s", rec.rateWindowSeconds},
          {"run_pixel_to_micron", optNumber(res.runPixelToMicron)}}},
        {"identity",
         {{"camera", rec.identity.camera},
          {"roi", rec.identity.roi},
          {"chip", rec.identity.chip},
          {"objective", rec.identity.objective}}},
        {"results",
         {{"valid_objects", res.validObjects},
          {"excluded_objects", res.excludedObjects},
          {"median_diameter_px", optNumber(res.medianDiameterPx)},
          {"mad_diameter_px", optNumber(res.madDiameterPx)},
          {"cv_diameter_percent", optNumber(res.cvDiameterPercent)},
          {"proposed_pixel_to_micron", optNumber(res.proposedPixelToMicron)},
          {"measured_diameter_um_at_run_factor", optNumber(res.measuredDiameterUmAtRunFactor)},
          {"run_factor_within_tolerance",
           res.runFactorWithinTolerance ? json(*res.runFactorWithinTolerance) : json(nullptr)},
          {"deformation_median", optNumber(res.deformationMedian)},
          {"event_rate",
           {{"status", res.eventRateStatus},
            {"windows", res.eventRateWindows},
            {"mean_per_s", optNumber(res.eventRateMeanPerS)},
            {"cv_percent", optNumber(res.eventRateCvPercent)}}},
          {"focus",
           {{"status", res.focusStatus},
            {"metric", res.focusLaplacianMedian ? json("laplacian_variance") : json(nullptr)},
            {"median", optNumber(res.focusLaplacianMedian)},
            {"objects", res.focusObjects}}},
          {"emodulus",
           {{"status", res.eStatus},
            {"median_kpa", optNumber(res.eMedianKpa)},
            {"nominal_kpa", optNumber(res.eNominalKpa)},
            {"ratio", optNumber(res.eRatio)},
            {"objects", res.eObjects}}}}},
        {"gates", gates},
        {"apply",
         rec.appliedNewFactor ? json{{"previous_factor", optNumber(rec.appliedPreviousFactor)},
                                     {"new_factor", optNumber(rec.appliedNewFactor)}}
                              : json(nullptr)},
        {"superseded_by", rec.supersededBy.empty() ? json(nullptr) : json(rec.supersededBy)},
    };
    return j.dump();
}

bool calibrationRecordFromJson(const std::string& text, CalibrationRecord& out, std::string* why)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return fail(why, "a record is not valid JSON");
    }
    if (!j.is_object()) return fail(why, "a record is not a JSON object");
    CalibrationRecord r;
    r.schemaVersion = static_cast<uint32_t>(getU64(j, "schema_version"));
    if (r.schemaVersion != kCalibrationRecordSchemaVersion)
        return fail(why, "record schema_version " + std::to_string(r.schemaVersion) + " is not supported");
    r.recordId = getString(j, "record_id");
    if (r.recordId.empty() || r.recordId.size() > kCalibrationRecordIdMaxBytes)
        return fail(why, "a record has no valid record_id");
    r.version = static_cast<uint32_t>(getU64(j, "version"));
    r.sequence = getU64(j, "sequence");
    const auto status = calibrationStatusFromString(getString(j, "status"));
    if (!status) return fail(why, "record " + r.recordId + " has an unknown status");
    r.status = *status;
    r.statusReason = getString(j, "status_reason");
    r.statusAtUnixMs = getU64(j, "status_at_unix_ms");
    r.createdAtUnixMs = getU64(j, "created_at_unix_ms");
    const json empty = json::object();
    const auto obj = [&](const char* key) -> const json& {
        const auto it = j.find(key);
        return it != j.end() && it->is_object() ? *it : empty;
    };
    const auto& src = obj("source");
    r.runId = getString(src, "run_id");
    r.file = getString(src, "file");
    r.fileSha256 = getString(src, "file_sha256");
    r.startGeneration = getU64(src, "start_generation");
    r.backgroundSha256 = getString(src, "background_sha256");
    const auto& m = obj("method");
    r.methodSource = getString(m, "source");
    r.methodId = getString(m, "method_id");
    r.revisionId = getString(m, "revision_id");
    r.revisionNumber = getU64(m, "revision_number");
    r.methodDisplayName = getString(m, "display_name");
    r.configSha256 = getString(m, "config_sha256");
    const auto& in = obj("inputs");
    r.reference.nominalDiameterUm = getOptNumber(in, "nominal_diameter_um").value_or(0.0);
    r.reference.diameterToleranceUm = getOptNumber(in, "diameter_tolerance_um").value_or(0.0);
    r.reference.nominalEKpa = getOptNumber(in, "nominal_e_kpa");
    r.reference.maxCvPercent = getOptNumber(in, "max_cv_percent").value_or(kCalibrationDefaultMaxCvPercent);
    r.minValidObjects = getU64(in, "min_valid_objects");
    r.rateWindowSeconds = getOptNumber(in, "rate_window_s").value_or(kCalibrationRateWindowSeconds);
    auto& res = r.derivation.results;
    res.runPixelToMicron = getOptNumber(in, "run_pixel_to_micron");
    const auto& id = obj("identity");
    r.identity.camera = getString(id, "camera");
    r.identity.roi = getString(id, "roi");
    r.identity.chip = getString(id, "chip");
    r.identity.objective = getString(id, "objective");
    const auto& rs = obj("results");
    res.validObjects = getU64(rs, "valid_objects");
    res.excludedObjects = getU64(rs, "excluded_objects");
    res.medianDiameterPx = getOptNumber(rs, "median_diameter_px");
    res.madDiameterPx = getOptNumber(rs, "mad_diameter_px");
    res.cvDiameterPercent = getOptNumber(rs, "cv_diameter_percent");
    res.proposedPixelToMicron = getOptNumber(rs, "proposed_pixel_to_micron");
    res.measuredDiameterUmAtRunFactor = getOptNumber(rs, "measured_diameter_um_at_run_factor");
    if (const auto it = rs.find("run_factor_within_tolerance"); it != rs.end() && it->is_boolean())
        res.runFactorWithinTolerance = it->get<bool>();
    res.deformationMedian = getOptNumber(rs, "deformation_median");
    if (const auto it = rs.find("event_rate"); it != rs.end() && it->is_object()) {
        res.eventRateStatus = getString(*it, "status");
        res.eventRateWindows = getU64(*it, "windows");
        res.eventRateMeanPerS = getOptNumber(*it, "mean_per_s");
        res.eventRateCvPercent = getOptNumber(*it, "cv_percent");
    }
    if (const auto it = rs.find("focus"); it != rs.end() && it->is_object()) {
        res.focusStatus = getString(*it, "status");
        res.focusLaplacianMedian = getOptNumber(*it, "median");
        res.focusObjects = getU64(*it, "objects");
    }
    if (const auto it = rs.find("emodulus"); it != rs.end() && it->is_object()) {
        res.eStatus = getString(*it, "status");
        res.eMedianKpa = getOptNumber(*it, "median_kpa");
        res.eNominalKpa = getOptNumber(*it, "nominal_kpa");
        res.eRatio = getOptNumber(*it, "ratio");
        res.eObjects = getU64(*it, "objects");
    }
    if (const auto it = j.find("gates"); it != j.end() && it->is_array()) {
        for (const auto& g : *it) {
            if (!g.is_object()) continue;
            CalibrationEvidenceGate gate;
            gate.id = getString(g, "id");
            gate.pass = g.value("pass", false);
            gate.value = getOptNumber(g, "value").value_or(0.0);
            gate.limit = getOptNumber(g, "limit").value_or(0.0);
            gate.reason = getString(g, "reason");
            r.derivation.gates.push_back(gate);
        }
    }
    if (const auto it = j.find("apply"); it != j.end() && it->is_object()) {
        r.appliedPreviousFactor = getOptNumber(*it, "previous_factor");
        r.appliedNewFactor = getOptNumber(*it, "new_factor");
    }
    r.supersededBy = getString(j, "superseded_by");
    out = std::move(r);
    return true;
}

CalibrationGateResult evaluateCalibrationGate(const CalibrationGateInput& in)
{
    CalibrationGateResult out;
    auto& g = out.gate;
    g.id = "calibration.record";
    const auto& spec = in.spec;
    const bool calibrationMethod = spec.purpose == MethodPurpose::Calibration;
    const auto policy = in.specError.empty() ? spec.policy : CalibrationPolicy::None;

    // The applied record and whether it is in effect, whatever the policy: the run snapshot
    // records the applied record that is in effect.
    std::string appliedState = "none"; // none | identity-unknown | identity-changed | factor-changed | in-effect
    std::string ageState = "-";        // fresh | expired | unknown
    double ageHours = 0.0;
    if (in.applied) {
        const auto& a = *in.applied;
        if (!in.current.known()) appliedState = "identity-unknown";
        else if (a.identity != in.current) appliedState = "identity-changed";
        else if (!a.appliedNewFactor || !sameCalibrationFactor(*a.appliedNewFactor, in.currentFactor))
            appliedState = "factor-changed";
        else appliedState = "in-effect";
        if (appliedState == "in-effect") {
            out.inEffectRecordId = a.recordId;
            if (in.nowUnixMs < a.statusAtUnixMs) {
                ageState = "unknown";
            } else {
                ageHours = static_cast<double>(in.nowUnixMs - a.statusAtUnixMs) / 3600000.0;
                ageState = ageHours > spec.maxAgeHours ? "expired" : "fresh";
            }
        }
    }
    // A proposed record this instrument could apply now (same identity).
    std::string applicableProposed;
    for (const auto& p : in.proposed) {
        if (p.applicable() && in.current.known() && p.identity == in.current) {
            applicableProposed = p.recordId;
            break;
        }
    }
    const std::string runIt = "run the calibration method";
    const std::string fixNone = applicableProposed.empty()
                                    ? runIt
                                    : "apply proposed record " + applicableProposed + " (calibration_apply), or " + runIt;
    const auto blocking = policy == CalibrationPolicy::Required ? GateStatus::Fail : GateStatus::Warn;
    auto appliedText = [&] {
        if (!in.applied) return std::string("no record applied");
        const auto& a = *in.applied;
        return "record " + a.recordId + " (factor " +
               (a.appliedNewFactor ? fmt(*a.appliedNewFactor, 10) : std::string("unknown")) + ", applied " +
               utcIsoFromUnixMs(a.statusAtUnixMs) + ")";
    };

    if (!in.specError.empty()) {
        g.status = GateStatus::Fail;
        g.reason = "the method's calibration members are malformed: " + in.specError;
        g.remediation = "fix purpose / calibration_policy / calibration_reference in the method's config.json";
    } else if (calibrationMethod) {
        g.status = GateStatus::NotRequired;
        g.reason = "a calibration method records the reference; it needs no applied record";
        g.detail = appliedText();
    } else if (policy == CalibrationPolicy::None) {
        g.status = GateStatus::NotRequired;
        g.reason = "the method's calibration policy is none";
        g.detail = appliedText();
    } else if (!in.storeError.empty()) {
        g.status = policy == CalibrationPolicy::Required ? GateStatus::Unavailable : GateStatus::Warn;
        g.reason = "the calibration store cannot be read: " + in.storeError;
        g.remediation = "repair or move aside the instrument calibration store, then " + runIt;
    } else if (appliedState == "none") {
        g.status = blocking;
        g.reason = "no calibration record is applied" + (in.invalidatedNote.empty() ? std::string() : " (" + in.invalidatedNote + ")");
        g.remediation = fixNone;
    } else if (appliedState == "identity-unknown") {
        g.status = policy == CalibrationPolicy::Required ? GateStatus::Unavailable : GateStatus::Warn;
        g.reason = "the camera or ROI identity is unknown, so the applied record cannot be confirmed";
        g.remediation = "connect the camera and let the ROI apply";
        g.detail = appliedText();
    } else if (appliedState == "identity-changed") {
        g.status = blocking;
        g.reason = "the applied record was derived under another setup (" +
                   describeIdentityChange(in.applied->identity, in.current) + ")";
        g.remediation = runIt;
        g.detail = appliedText();
    } else if (appliedState == "factor-changed") {
        g.status = blocking;
        g.reason = "the applied record's factor " +
                   (in.applied->appliedNewFactor ? fmt(*in.applied->appliedNewFactor, 10) : std::string("unknown")) +
                   " is not the current pixel-to-micron factor " + fmt(in.currentFactor, 10) +
                   " (it changed after the record was applied)";
        g.remediation = "re-apply record " + in.applied->recordId + " (calibration_apply), or " + runIt;
        g.detail = appliedText();
    } else if (ageState == "unknown") {
        g.status = GateStatus::Warn;
        g.reason = "the applied record's age cannot be determined (the clock reads earlier than its apply time)";
        g.remediation = runIt;
        g.detail = appliedText();
    } else if (ageState == "expired") {
        g.status = GateStatus::Warn;
        g.reason = "the applied record is " + fmt(ageHours, 3) + " h old (the method allows " + fmt(spec.maxAgeHours, 4) +
                   " h)";
        g.remediation = runIt;
        g.detail = appliedText();
    } else {
        g.status = GateStatus::Pass;
        g.detail = appliedText() + ", " + fmt(ageHours, 3) + " h old";
    }

    std::ostringstream key;
    key << toString(spec.purpose) << '|' << toString(policy) << '|' << spec.maxAgeHours << '|' << in.specError << '|'
        << (in.applied ? in.applied->recordId + "#" + std::to_string(in.applied->version) : std::string("-")) << '|'
        << appliedState << '|' << ageState << '|' << applicableProposed << '|' << in.storeError;
    out.invalidationKey = key.str();
    return out;
}

} // namespace backend::app
