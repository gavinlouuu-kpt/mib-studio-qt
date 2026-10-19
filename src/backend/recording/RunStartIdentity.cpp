#include "backend/recording/RunStartIdentity.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <limits>
#include <set>

namespace backend::recording {

namespace {

// Plain nlohmann::json: the wheel build compiles this against nlohmann_json 3.6 (EPEL 8), which has no
// ordered_json (3.9). Objects therefore keep their keys sorted, the free key/values included.
using OJson = nlohmann::json;

bool fail(std::string* why, std::string message)
{
    if (why) *why = std::move(message);
    return false;
}

// A key as it may appear in a refusal: bounded, so a refusal never echoes a large client string.
std::string shown(const std::string& key)
{
    return key.size() > 40 ? key.substr(0, 40) + "..." : key;
}

bool validUtf8(const std::string& s)
{
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + extra >= n) return false;   // truncated sequence
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and values above U+10FFFF are not UTF-8.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

// A declared string: 1..max bytes of UTF-8 without control characters (notes may hold \t \n \r).
bool checkText(const std::string& where, const std::string& v, std::size_t max, bool multiline, std::string* why)
{
    if (v.empty()) return fail(why, where + ": empty; use null when it is not declared");
    if (v.size() > max)
        return fail(why, where + ": " + std::to_string(v.size()) + " bytes, at most " + std::to_string(max));
    if (!validUtf8(v)) return fail(why, where + ": not valid UTF-8");
    for (const char ch : v) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) {
            if (multiline && (c == '\n' || c == '\r' || c == '\t')) continue;
            return fail(why, where + ": control characters are not allowed");
        }
    }
    return true;
}

bool canonicalLowerUuid(const std::string& s)
{
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- strict request parsing (types and shape; bounds are validateStartRequest's) ------------------

bool takeString(const OJson& v, const std::string& where, std::optional<std::string>& out, std::string* why)
{
    if (v.is_null()) return true;
    if (!v.is_string()) return fail(why, where + ": expected a string or null");
    out = v.get<std::string>();
    return true;
}

bool takeNumber(const OJson& v, const std::string& where, std::optional<double>& out, std::string* why)
{
    if (v.is_null()) return true;
    if (!v.is_number()) return fail(why, where + ": expected a number or null");
    out = v.get<double>();
    return true;
}

bool parseOperator(const OJson& v, std::optional<OperatorRecord>& out, std::string* why)
{
    if (v.is_null()) return true;
    if (!v.is_object()) return fail(why, "operator: expected an object {id, name} or null");
    OperatorRecord op;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it.key() == "id") {
            if (!takeString(it.value(), "operator.id", op.id, why)) return false;
        } else if (it.key() == "name") {
            if (!takeString(it.value(), "operator.name", op.name, why)) return false;
        } else {
            return fail(why, "operator: unknown key \"" + shown(it.key()) + "\"");
        }
    }
    out = std::move(op);
    return true;
}

bool parseAnnotation(const OJson& v, std::optional<ExperimentAnnotation>& out, std::string* why)
{
    if (v.is_null()) return true;
    if (!v.is_object()) return fail(why, "annotation: expected an object or null");
    ExperimentAnnotation a;
    for (auto it = v.begin(); it != v.end(); ++it) {
        const std::string& key = it.key();
        const OJson& value = it.value();
        bool ok = true;
        if (key == "sample") ok = takeString(value, "annotation.sample", a.sample, why);
        else if (key == "condition") ok = takeString(value, "annotation.condition", a.condition, why);
        else if (key == "experimental_unit") ok = takeString(value, "annotation.experimental_unit", a.experimentalUnit, why);
        else if (key == "channel") ok = takeString(value, "annotation.channel", a.channel, why);
        else if (key == "medium") ok = takeString(value, "annotation.medium", a.medium, why);
        else if (key == "notes") ok = takeString(value, "annotation.notes", a.notes, why);
        else if (key == "temperature_c") ok = takeNumber(value, "annotation.temperature_c", a.temperatureC, why);
        else if (key == "concentration") {
            if (value.is_null()) continue;
            if (!value.is_object())
                return fail(why, "annotation.concentration: expected an object {value, unit} or null");
            std::optional<double> number;
            std::optional<std::string> unit;
            for (auto c = value.begin(); c != value.end(); ++c) {
                if (c.key() == "value") ok = takeNumber(c.value(), "annotation.concentration.value", number, why);
                else if (c.key() == "unit") ok = takeString(c.value(), "annotation.concentration.unit", unit, why);
                else return fail(why, "annotation.concentration: unknown key \"" + shown(c.key()) + "\"");
                if (!ok) return false;
            }
            if (!number || !unit)
                return fail(why, "annotation.concentration: needs both value and unit (or null when not declared)");
            a.concentration = ConcentrationRecord{*number, *unit};
        } else if (key == "free") {
            if (value.is_null()) continue;
            if (!value.is_object()) return fail(why, "annotation.free: expected an object of string values");
            if (value.size() > kAnnotationFreeMaxEntries)
                return fail(why, "annotation.free: " + std::to_string(value.size()) + " entries, at most " +
                                     std::to_string(kAnnotationFreeMaxEntries));
            for (auto f = value.begin(); f != value.end(); ++f) {
                if (!f.value().is_string())
                    return fail(why, "annotation.free[\"" + shown(f.key()) + "\"]: values are strings only");
                a.free.emplace_back(f.key(), f.value().get<std::string>());
            }
        } else {
            return fail(why, "annotation: unknown key \"" + shown(key) + "\"");
        }
        if (!ok) return false;
    }
    out = std::move(a);
    return true;
}

// ---- tolerant annotation reading ---------------------------------------------------------------------

// An empty string reads as unknown, like a missing one.
std::optional<std::string> readStr(const OJson& j, const char* key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    std::string v = it->get<std::string>();
    if (v.empty()) return std::nullopt;
    return v;
}

std::optional<double> readNum(const OJson& j, const char* key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return std::nullopt;
    return it->get<double>();
}

// An empty string is never a value: written as null.
OJson opt(const std::optional<std::string>& v)
{
    return v && !v->empty() ? OJson(*v) : OJson(nullptr);
}

} // namespace

bool validateStartRequest(const StartRequestIdentity& request, std::string* why)
{
    const auto& id = request.identity;
    if (id.experimentId && !canonicalLowerUuid(*id.experimentId))
        return fail(why, "experiment_id: expected a UUID (8-4-4-4-12 hex digits)");
    if (id.runIndex && !id.experimentId) return fail(why, "run_index: needs an experiment_id");
    if (id.sampleRef) {
        if (!id.experimentId) return fail(why, "sample_ref: needs an experiment_id (it points into its sample sheet)");
        if (!checkText("sample_ref", *id.sampleRef, kStartIdMaxBytes, false, why)) return false;
    }
    if (id.selectedOperator) {
        const auto& op = *id.selectedOperator;
        if (!op.id && !op.name) return fail(why, "operator: give an id or a name, or null when none is selected");
        if (op.id && !checkText("operator.id", *op.id, kStartIdMaxBytes, false, why)) return false;
        if (op.name && !checkText("operator.name", *op.name, kStartIdMaxBytes, false, why)) return false;
    }
    if (id.calibrationRecordId &&
        !checkText("calibration_record_id", *id.calibrationRecordId, kStartIdMaxBytes, false, why))
        return false;

    if (!request.annotation) return true;
    const auto& a = *request.annotation;
    const struct {
        const char* where;
        const std::optional<std::string>& value;
    } texts[] = {{"annotation.sample", a.sample},
                 {"annotation.condition", a.condition},
                 {"annotation.experimental_unit", a.experimentalUnit},
                 {"annotation.channel", a.channel},
                 {"annotation.medium", a.medium}};
    for (const auto& t : texts)
        if (t.value && !checkText(t.where, *t.value, kAnnotationTextMaxBytes, false, why)) return false;
    if (a.notes && !checkText("annotation.notes", *a.notes, kAnnotationNotesMaxBytes, true, why)) return false;
    if (a.concentration) {
        if (!std::isfinite(a.concentration->value) || a.concentration->value < 0.0)
            return fail(why, "annotation.concentration.value: expected a finite number >= 0");
        if (!checkText("annotation.concentration.unit", a.concentration->unit, kAnnotationUnitMaxBytes, false, why))
            return false;
    }
    if (a.temperatureC &&
        (!std::isfinite(*a.temperatureC) || *a.temperatureC < kAnnotationTemperatureMinC ||
         *a.temperatureC > kAnnotationTemperatureMaxC))
        return fail(why, "annotation.temperature_c: expected degrees Celsius from " +
                             std::to_string(kAnnotationTemperatureMinC) + " to " +
                             std::to_string(kAnnotationTemperatureMaxC));
    if (a.free.size() > kAnnotationFreeMaxEntries)
        return fail(why, "annotation.free: " + std::to_string(a.free.size()) + " entries, at most " +
                             std::to_string(kAnnotationFreeMaxEntries));
    std::set<std::string> keys;
    for (const auto& [key, value] : a.free) {
        if (!checkText("annotation.free key", key, kAnnotationFreeKeyMaxBytes, false, why)) return false;
        if (!keys.insert(key).second) return fail(why, "annotation.free: duplicate key \"" + shown(key) + "\"");
        if (!checkText("annotation.free[\"" + shown(key) + "\"]", value, kAnnotationFreeValueMaxBytes, false, why))
            return false;
    }
    return true;
}

bool parseStartRequestJson(const std::string& text, StartRequestIdentity& out, std::string* why)
{
    out = StartRequestIdentity{};
    if (text.empty()) return true;
    if (text.size() > kStartRequestMaxBytes)
        return fail(why, "startRequest: " + std::to_string(text.size()) + " bytes, at most " +
                             std::to_string(kStartRequestMaxBytes));
    // Duplicate keys are refused: nlohmann keeps one of them silently, so a repeated key would record
    // a value the operator did not look at.
    std::vector<std::set<std::string>> keys;
    std::string duplicate;
    const auto callback = [&](int, nlohmann::json::parse_event_t event, OJson& parsed) {
        using E = nlohmann::json::parse_event_t;
        if (event == E::object_start) keys.emplace_back();
        else if (event == E::object_end && !keys.empty()) keys.pop_back();
        else if (event == E::key && !keys.empty() && !keys.back().insert(parsed.get<std::string>()).second &&
                 duplicate.empty())
            duplicate = parsed.get<std::string>();
        return true;
    };
    OJson root;
    try {
        root = OJson::parse(text, callback);
    } catch (const nlohmann::json::parse_error& e) {
        return fail(why, "startRequest: not valid JSON (at byte " + std::to_string(e.byte) + ")");
    } catch (const std::exception&) {
        return fail(why, "startRequest: not valid JSON");
    }
    if (!duplicate.empty()) return fail(why, "startRequest: duplicate key \"" + shown(duplicate) + "\"");
    if (root.is_null()) return true;
    if (!root.is_object()) return fail(why, "startRequest: expected a JSON object");

    StartRequestIdentity parsed;
    auto& id = parsed.identity;
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& key = it.key();
        const OJson& value = it.value();
        if (key == "experiment_id") {
            if (!takeString(value, "experiment_id", id.experimentId, why)) return false;
            if (id.experimentId) id.experimentId = lower(*id.experimentId);
        } else if (key == "run_index") {
            if (value.is_null()) continue;
            if (!value.is_number_integer())
                return fail(why, "run_index: expected a whole number from 0 to 4294967295, or null");
            if (value.is_number_unsigned()) {
                const auto v = value.get<uint64_t>();
                if (v > std::numeric_limits<uint32_t>::max())
                    return fail(why, "run_index: expected a whole number from 0 to 4294967295, or null");
                id.runIndex = static_cast<uint32_t>(v);
            } else {
                const auto v = value.get<int64_t>();
                if (v < 0 || v > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                    return fail(why, "run_index: expected a whole number from 0 to 4294967295, or null");
                id.runIndex = static_cast<uint32_t>(v);
            }
        } else if (key == "sample_ref") {
            if (!takeString(value, "sample_ref", id.sampleRef, why)) return false;
        } else if (key == "operator") {
            if (!parseOperator(value, id.selectedOperator, why)) return false;
        } else if (key == "calibration_record_id") {
            if (!takeString(value, "calibration_record_id", id.calibrationRecordId, why)) return false;
        } else if (key == "annotation") {
            if (!parseAnnotation(value, parsed.annotation, why)) return false;
        } else {
            return fail(why, "startRequest: unknown key \"" + shown(key) + "\"");
        }
    }
    if (!validateStartRequest(parsed, why)) return false;
    out = std::move(parsed);
    return true;
}

std::string experimentAnnotationToJson(const ExperimentAnnotation& a)
{
    OJson j;
    j["schema_version"] = kExperimentAnnotationSchemaVersion;
    j["sample"] = opt(a.sample);
    j["condition"] = opt(a.condition);
    j["experimental_unit"] = opt(a.experimentalUnit);
    if (a.concentration && std::isfinite(a.concentration->value) && !a.concentration->unit.empty()) {
        OJson c;
        c["value"] = a.concentration->value;
        c["unit"] = a.concentration->unit;
        j["concentration"] = std::move(c);
    } else {
        j["concentration"] = nullptr;
    }
    j["channel"] = opt(a.channel);
    j["medium"] = opt(a.medium);
    j["temperature_c"] = a.temperatureC && std::isfinite(*a.temperatureC) ? OJson(*a.temperatureC) : OJson(nullptr);
    j["notes"] = opt(a.notes);
    OJson free = OJson::object();
    for (const auto& [key, value] : a.free)
        if (!key.empty() && !value.empty()) free[key] = value;
    j["free"] = std::move(free);
    return j.dump();
}

std::optional<ExperimentAnnotation> parseExperimentAnnotation(const std::string& json, std::string* why)
{
    const OJson root = OJson::parse(json, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object()) {
        if (why) *why = "annotation_json is not a JSON object";
        return std::nullopt;
    }
    const OJson& flat = root;
    ExperimentAnnotation a;
    a.sample = readStr(flat, "sample");
    a.condition = readStr(flat, "condition");
    a.experimentalUnit = readStr(flat, "experimental_unit");
    a.channel = readStr(flat, "channel");
    a.medium = readStr(flat, "medium");
    a.temperatureC = readNum(flat, "temperature_c");
    a.notes = readStr(flat, "notes");
    const auto c = flat.find("concentration");
    if (c != flat.end() && c->is_object()) {
        const auto value = readNum(*c, "value");
        const auto unit = readStr(*c, "unit");
        if (value && unit) a.concentration = ConcentrationRecord{*value, *unit};   // readStr: no "" unit
    }
    const auto free = root.find("free");
    if (free != root.end() && free->is_object()) {
        for (auto it = free->begin(); it != free->end(); ++it) {
            if (it.key().empty() || !it.value().is_string()) continue;
            std::string value = it.value().get<std::string>();
            if (!value.empty()) a.free.emplace_back(it.key(), std::move(value));
        }
    }
    return a;
}

} // namespace backend::recording
