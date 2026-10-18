#include "backend/recording/RunProvenanceRecord.h"

#include "backend/recording/Hdf5Service.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace backend::recording {

namespace {

// Plain nlohmann::json (keys sorted on dump, so still a stable order): the wheel build compiles this
// file against nlohmann_json 3.6 (EPEL 8), which has no ordered_json (3.9).
using OJson = nlohmann::json;
using Json = nlohmann::json;

// ---- writing -----------------------------------------------------------------

template <typename T>
OJson opt(const std::optional<T>& v)
{
    return v ? OJson(*v) : OJson(nullptr);
}

OJson opt(const std::optional<double>& v)
{
    return v && std::isfinite(*v) ? OJson(*v) : OJson(nullptr);
}

// An empty string is never a value: written as null.
OJson opt(const std::optional<std::string>& v)
{
    return v && !v->empty() ? OJson(*v) : OJson(nullptr);
}

OJson text(const std::string& v)
{
    return v.empty() ? OJson(nullptr) : OJson(v);
}

OJson lutJson(const EModulusLutRecord& l)
{
    OJson j;
    j["loaded"] = l.loaded;
    j["lut_id"] = opt(l.lutId);
    j["revision"] = opt(l.revision);
    j["source"] = opt(l.source);
    j["sha256"] = opt(l.sha256);
    j["catalog_sha256"] = opt(l.catalogSha256);
    j["checksum_status"] = opt(l.checksumStatus);
    j["catalog_matches_loaded"] = opt(l.catalogMatchesLoaded());
    return j;
}

OJson pumpJson(const PumpRecord& p)
{
    OJson j;
    j["slot"] = text(p.slot);
    j["connected"] = opt(p.connected);
    if (!p.connected.value_or(false)) return j; // no setpoint or readback for an absent pump
    j["model"] = opt(p.model);
    j["port"] = opt(p.port);
    j["modbus_address"] = opt(p.modbusAddress);
    OJson set;
    set["flow_rate"] = opt(p.flowRate);
    set["flow_rate_unit"] = opt(p.flowRateUnit);
    set["flow_rate_unit_code"] = opt(p.flowRateUnitCode);
    set["direction"] = opt(p.direction);
    set["microliters_per_rev"] = opt(p.microlitersPerRev);
    j["setpoint"] = std::move(set);
    if (p.readback) {
        const auto& r = *p.readback;
        OJson rb;
        rb["age_us"] = r.ageUs;
        rb["last_poll_ok"] = opt(r.lastPollOk);
        rb["run_status"] = text(r.runStatus);
        rb["speed_rpm"] = opt(r.speedRpm);
        rb["realtime_flow_raw"] = opt(r.realtimeFlowRaw);
        rb["accumulated_volume_raw"] = opt(r.accumulatedVolumeRaw);
        rb["stalled"] = opt(r.stalled);
        j["readback"] = std::move(rb);
    } else {
        j["readback"] = nullptr;
    }
    return j;
}

OJson ledJson(const LedRecord& l)
{
    OJson j;
    j["source"] = opt(l.source);
    j["error"] = opt(l.error);
    j["enabled"] = opt(l.enabled());
    j["control"] = opt(l.control);
    j["delay_us"] = opt(l.delayUs);
    j["width_us"] = opt(l.widthUs);
    j["clock_khz"] = opt(l.clockKhz);
    j["stopped_run_at_start"] = opt(l.stoppedRunAtStart);
    return j;
}

OJson stageJson(const StageRecord& s)
{
    OJson j;
    j["connected"] = s.connected;
    if (!s.connected) return j;
    j["model"] = opt(s.model);
    j["serial"] = opt(s.serial);
    j["firmware"] = opt(s.firmware);
    j["zero_set"] = opt(s.zeroSet);
    if (s.readback) {
        const auto& r = *s.readback;
        OJson rb;
        rb["age_us"] = r.ageUs;
        rb["state"] = opt(r.state);
        rb["position_um"] = opt(r.positionUm);
        rb["limit_positive"] = opt(r.limitPositive);
        rb["limit_negative"] = opt(r.limitNegative);
        rb["home"] = opt(r.home);
        rb["emergency_stop"] = opt(r.emergencyStop);
        rb["driver_alarm"] = opt(r.driverAlarm);
        j["readback"] = std::move(rb);
    } else {
        j["readback"] = nullptr;
    }
    return j;
}

OJson piezoJson(const PiezoRecord& p)
{
    OJson j;
    j["connected"] = p.connected;
    if (!p.connected) return j;
    j["voltage_v"] = opt(p.voltageV);
    j["voltage_basis"] = "last_read_or_written";
    j["autofocus_enabled"] = opt(p.autofocusEnabled);
    return j;
}

// ---- reading -----------------------------------------------------------------

const Json* member(const Json& j, const char* key)
{
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return nullptr;
    return &*it;
}

// An empty string reads as unknown, like a missing one.
std::optional<std::string> str(const Json& j, const char* key)
{
    const Json* m = member(j, key);
    if (!m || !m->is_string()) return std::nullopt;
    std::string v = m->get<std::string>();
    if (v.empty()) return std::nullopt;
    return v;
}

std::optional<double> num(const Json& j, const char* key)
{
    const Json* m = member(j, key);
    if (!m || !m->is_number()) return std::nullopt;
    return m->get<double>();
}

std::optional<bool> flag(const Json& j, const char* key)
{
    const Json* m = member(j, key);
    if (!m || !m->is_boolean()) return std::nullopt;
    return m->get<bool>();
}

std::optional<uint64_t> u64(const Json& j, const char* key)
{
    const Json* m = member(j, key);
    if (!m || !m->is_number_unsigned()) {
        if (m && m->is_number_integer() && m->get<int64_t>() >= 0) return static_cast<uint64_t>(m->get<int64_t>());
        return std::nullopt;
    }
    return m->get<uint64_t>();
}

std::optional<int> i32(const Json& j, const char* key)
{
    const Json* m = member(j, key);
    if (!m || !m->is_number_integer()) return std::nullopt;
    return static_cast<int>(m->get<int64_t>());
}

std::optional<EModulusLutRecord> parseLut(const Json& root)
{
    const Json* j = member(root, "emodulus_lut");
    if (!j || !j->is_object()) return std::nullopt;
    const auto loaded = flag(*j, "loaded");
    if (!loaded) return std::nullopt;
    EModulusLutRecord l;
    l.loaded = *loaded;
    l.lutId = str(*j, "lut_id");
    l.revision = str(*j, "revision");
    l.source = str(*j, "source");
    l.sha256 = str(*j, "sha256");
    l.catalogSha256 = str(*j, "catalog_sha256");
    l.checksumStatus = str(*j, "checksum_status");
    return l;
}

std::optional<std::vector<PumpRecord>> parsePumps(const Json& root)
{
    const Json* j = member(root, "pumps");
    if (!j || !j->is_array()) return std::nullopt;
    std::vector<PumpRecord> pumps;
    for (const auto& e : *j) {
        if (!e.is_object()) continue;
        PumpRecord p;
        const auto slot = str(e, "slot");
        if (!slot) continue;   // an entry that names no slot describes no pump
        p.slot = *slot;
        p.connected = flag(e, "connected");
        p.model = str(e, "model");
        p.port = str(e, "port");
        p.modbusAddress = i32(e, "modbus_address");
        if (const Json* s = member(e, "setpoint"); s && s->is_object()) {
            p.flowRate = num(*s, "flow_rate");
            p.flowRateUnit = str(*s, "flow_rate_unit");
            p.flowRateUnitCode = i32(*s, "flow_rate_unit_code");
            p.direction = str(*s, "direction");
            p.microlitersPerRev = num(*s, "microliters_per_rev");
        }
        if (const Json* r = member(e, "readback"); r && r->is_object()) {
            const auto age = u64(*r, "age_us");
            const auto status = str(*r, "run_status");
            if (age && status) {
                PumpReadbackRecord rb;
                rb.ageUs = *age;
                rb.runStatus = *status;
                rb.lastPollOk = flag(*r, "last_poll_ok");
                rb.speedRpm = num(*r, "speed_rpm");
                rb.realtimeFlowRaw = num(*r, "realtime_flow_raw");
                rb.accumulatedVolumeRaw = num(*r, "accumulated_volume_raw");
                rb.stalled = flag(*r, "stalled");
                p.readback = rb;
            }
        }
        pumps.push_back(std::move(p));
    }
    return pumps;
}

std::optional<LedRecord> parseLed(const Json& root)
{
    const Json* j = member(root, "led");
    if (!j || !j->is_object()) return std::nullopt;
    LedRecord l;
    l.source = str(*j, "source");
    l.error = str(*j, "error");
    if (const auto c = u64(*j, "control")) l.control = static_cast<uint32_t>(*c);
    l.delayUs = num(*j, "delay_us");
    l.widthUs = num(*j, "width_us");
    if (const auto k = u64(*j, "clock_khz")) l.clockKhz = static_cast<uint32_t>(*k);
    l.stoppedRunAtStart = flag(*j, "stopped_run_at_start");
    return l;
}

std::optional<StageRecord> parseStage(const Json& root)
{
    const Json* j = member(root, "stage");
    if (!j || !j->is_object()) return std::nullopt;
    const auto connected = flag(*j, "connected");
    if (!connected) return std::nullopt;
    StageRecord s;
    s.connected = *connected;
    s.model = str(*j, "model");
    s.serial = str(*j, "serial");
    s.firmware = str(*j, "firmware");
    s.zeroSet = flag(*j, "zero_set");
    if (const Json* r = member(*j, "readback"); r && r->is_object()) {
        if (const auto age = u64(*r, "age_us")) {
            StageReadbackRecord rb;
            rb.ageUs = *age;
            rb.state = str(*r, "state");
            rb.positionUm = num(*r, "position_um");
            rb.limitPositive = flag(*r, "limit_positive");
            rb.limitNegative = flag(*r, "limit_negative");
            rb.home = flag(*r, "home");
            rb.emergencyStop = flag(*r, "emergency_stop");
            rb.driverAlarm = flag(*r, "driver_alarm");
            s.readback = rb;
        }
    }
    return s;
}

std::optional<PiezoRecord> parsePiezo(const Json& root)
{
    const Json* j = member(root, "piezo");
    if (!j || !j->is_object()) return std::nullopt;
    const auto connected = flag(*j, "connected");
    if (!connected) return std::nullopt;
    PiezoRecord p;
    p.connected = *connected;
    p.voltageV = num(*j, "voltage_v");
    p.autofocusEnabled = flag(*j, "autofocus_enabled");
    return p;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::optional<bool> EModulusLutRecord::catalogMatchesLoaded() const
{
    if (!sha256 || !catalogSha256 || sha256->empty() || catalogSha256->empty()) return std::nullopt;
    return lower(*sha256) == lower(*catalogSha256);
}

std::optional<bool> LedRecord::enabled() const
{
    if (!control) return std::nullopt;
    return *control != 0;
}

std::string runProvenanceV3Members(const RunProvenanceV3& v3)
{
    OJson j;
    j["run_id"] = opt(v3.runId);
    j["emodulus_lut"] = v3.eModulusLut ? lutJson(*v3.eModulusLut) : OJson(nullptr);
    if (v3.pumps) {
        OJson pumps = OJson::array();
        for (const auto& p : *v3.pumps) pumps.push_back(pumpJson(p));
        j["pumps"] = std::move(pumps);
    } else {
        j["pumps"] = nullptr;
    }
    j["led"] = v3.led ? ledJson(*v3.led) : OJson(nullptr);
    j["stage"] = v3.stage ? stageJson(*v3.stage) : OJson(nullptr);
    j["piezo"] = v3.piezo ? piezoJson(*v3.piezo) : OJson(nullptr);
    j["pl_run_id"] = opt(v3.plRunId);
    std::string text = j.dump();
    // Strip the braces: the caller splices the members into the snapshot object.
    return text.size() >= 2 ? text.substr(1, text.size() - 2) : std::string{};
}

bool parseRunProvenance(const std::string& runSnapshotJson, RecordedRunProvenance& out, std::string* why)
{
    out.schemaVersion = 0;
    out.v3 = RunProvenanceV3{};
    Json root = Json::parse(runSnapshotJson, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        if (why) *why = "run_snapshot_json is not a JSON object";
        return false;
    }
    const auto version = u64(root, "schema_version");
    // A snapshot without a version predates the field; read it as v1 (nothing it lacks is invented).
    out.schemaVersion = version ? static_cast<uint32_t>(*version) : 1u;
    auto& v = out.v3;
    v.runId = str(root, "run_id");
    v.eModulusLut = parseLut(root);
    v.pumps = parsePumps(root);
    v.led = parseLed(root);
    v.stage = parseStage(root);
    v.piezo = parsePiezo(root);
    v.plRunId = u64(root, "pl_run_id");
    return true;
}

std::string runStopToJson(const RunStopRecord& stop)
{
    OJson j;
    j["reason"] = stop.reason;
    j["message"] = opt(stop.message);
    return j.dump();
}

std::optional<RunStopRecord> parseRunStop(const std::string& json, std::string* why)
{
    Json root = Json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        if (why) *why = "run_stop_json is not a JSON object";
        return std::nullopt;
    }
    const auto reason = str(root, "reason");
    if (!reason || reason->empty()) {
        if (why) *why = "run_stop_json has no reason";
        return std::nullopt;
    }
    RunStopRecord stop;
    stop.reason = *reason;
    stop.message = str(root, "message");
    return stop;
}

bool readRecordedRunProvenance(const services::Hdf5Service& file, RecordedRunProvenance& out, std::string* why)
{
    out = RecordedRunProvenance{};
    std::string stopJson;
    if (file.readRunStopJson(stopJson)) out.stop = parseRunStop(stopJson);
    std::string snapshot;
    if (!file.readRunSnapshotJson(snapshot)) {
        if (why) *why = "the file has no run snapshot";
        return false;
    }
    RecordedRunProvenance parsed;
    if (!parseRunProvenance(snapshot, parsed, why)) return false;
    out.schemaVersion = parsed.schemaVersion;
    out.v3 = std::move(parsed.v3);
    return true;
}

} // namespace backend::recording
