#include "backend/app/HostSettings.h"

#include "backend/services/SerialPortClaims.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define MIB_GETPID _getpid
#else
#include <unistd.h>
#define MIB_GETPID getpid
#endif

namespace backend::app {
namespace {
using Json = nlohmann::json;
namespace claims = services::portclaims;

constexpr int kVersion = 1;
constexpr const char* kBootKey = "boot_disabled_services";
constexpr const char* kSlots[] = {"pump_sample_", "pump_sheath_"};
// dLSP volume / flow unit codes (SyringePumpTab / SyringePumpSettingsDialog): 100 = uL, 103 = mL.
constexpr int kUnitUl = 100;
constexpr int kUnitMl = 103;
constexpr int kUnitRpm = 200; // desktop-only head speed of the peristaltic pump (pumpRate.ts RPM_UNIT)

enum class Kind { Int, Number, Text };
struct Spec {
    const char* suffix;
    Kind kind;
    double low;
    double high;
};
// The Qt keys (SyringePumpSettingsDialog::saveConfig, SyringePumpTab::saveConfig), plus `port`: the exact endpoint
// text, because the desktop app also takes system paths (`/dev/ttyUSB0`) that an integer COM number cannot name.
// Ranges are the widest the dialogs offer; the desktop app narrows them further.
constexpr Spec kSpecs[] = {
    {"com_port", Kind::Int, 1, 65535},       {"port", Kind::Text, 1, 512},
    {"baud_rate", Kind::Int, 1, 4000000},    {"address", Kind::Int, 1, 247},
    {"syringe_vol", Kind::Number, 0.1, 9999}, {"syringe_unit", Kind::Int, 0, 65535},
    {"flow_rate", Kind::Number, 0, 100000},  {"flow_unit", Kind::Int, 0, 65535},
    {"direction", Kind::Int, 0, 1},
};

std::string normaliseToken(std::string token) {
    const auto first = token.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    token = token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(token.begin(), token.end(), '-', '_');
    return token;
}

// Returns an error text, or empty when `value` is valid for `key`. A key outside the table is an error.
std::string checkPumpValue(const std::string& key, const Json& value) {
    for (const char* slot : kSlots) {
        if (key.rfind(slot, 0) != 0) continue;
        const std::string suffix = key.substr(std::string(slot).size());
        for (const auto& spec : kSpecs) {
            if (suffix != spec.suffix) continue;
            switch (spec.kind) {
            case Kind::Int:
                if (!value.is_number_integer()) return key + " must be a whole number";
                break;
            case Kind::Number:
                if (!value.is_number() || !std::isfinite(value.get<double>())) return key + " must be a number";
                break;
            case Kind::Text:
                if (!value.is_string()) return key + " must be text";
                {
                    const auto text = value.get<std::string>();
                    if (text.find('\0') != std::string::npos || text.find_first_not_of(" \t") == std::string::npos ||
                        text.size() > static_cast<std::size_t>(spec.high))
                        return key + " is not a valid serial port name";
                }
                return {};
            }
            const double number = value.get<double>();
            if (number < spec.low || number > spec.high) return key + " is out of range";
            if (suffix == "syringe_unit" && number != kUnitUl && number != kUnitMl) return key + " must be 100 (uL) or 103 (mL)";
            // The rate unit also takes the desktop app's head-speed unit for a peristaltic pump.
            if (suffix == "flow_unit" && number != kUnitUl && number != kUnitMl && number != kUnitRpm)
                return key + " must be 100 (uL/min), 103 (mL/min) or 200 (rpm)";
            return {};
        }
    }
    return "unknown setting " + key;
}

Json readDocument(const std::string& path, std::string& error) {
    error.clear();
    if (path.empty()) return Json::object();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return Json::object();
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot read " + path; return Json::object(); }
    std::stringstream text;
    text << in.rdbuf();
    if (text.str().size() > 1024 * 1024) { error = "host settings file is larger than 1 MiB"; return Json::object(); }
    const auto parsed = Json::parse(text.str(), nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) { error = "host settings file is not a JSON object"; return Json::object(); }
    // Tolerant like Qt's loadConfig: keep the keys that are valid, leave out the rest.
    Json kept = Json::object();
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        if (it.key() == kBootKey) {
            if (!it.value().is_array()) continue;
            Json tokens = Json::array();
            for (const auto& item : it.value())
                if (item.is_string()) {
                    const auto token = normaliseToken(item.get<std::string>());
                    if (!token.empty() && std::find(tokens.begin(), tokens.end(), token) == tokens.end()) tokens.push_back(token);
                }
            if (!tokens.empty()) kept[kBootKey] = tokens;
        } else if (checkPumpValue(it.key(), it.value()).empty()) {
            kept[it.key()] = it.value();
        }
    }
    return kept;
}
} // namespace

HostSettingsStore::HostSettingsStore(std::string path) : path_(std::move(path)) {}

void HostSettingsStore::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    doc_ = readDocument(path_, loadError_);
    if (!loadError_.empty()) SPDLOG_WARN("HostSettings: {} ({})", loadError_, path_);
}

std::string HostSettingsStore::loadError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadError_;
}

std::string HostSettingsStore::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

Json HostSettingsStore::document() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return doc_;
}

HostSettingsStore::Result HostSettingsStore::writeLocked(const Json& next) {
    if (path_.empty()) return {false, "There is no data directory to keep the settings in"};
    Json out = next;
    out["version"] = kVersion;
    static std::atomic<unsigned long long> counter{0};
    const std::filesystem::path target(path_);
    auto temp = target;
    temp += ".tmp." + std::to_string(MIB_GETPID()) + "." + std::to_string(++counter);
    std::error_code ec;
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return {false, "Cannot write " + temp.string()};
        const auto text = out.dump(2) + "\n";
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) { file.close(); std::filesystem::remove(temp, ec); return {false, "Cannot write " + temp.string()}; }
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return {false, "Cannot replace " + target.string() + ": " + ec.message()};
    }
    doc_ = next;
    loadError_.clear();
    return {true, "Saved"};
}

HostSettingsStore::Result HostSettingsStore::setPumps(const Json& patch) {
    if (!patch.is_object() || patch.empty()) return {false, "Pump settings must be a non-empty object"};
    Json next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = doc_;
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            if (it.key() == kBootKey) return {false, std::string(kBootKey) + " is not a pump setting"};
            if (it.value().is_null()) {
                // Only a known pump key can be removed.
                const std::string probe = it.key();
                bool known = false;
                for (const char* slot : kSlots)
                    if (probe.rfind(slot, 0) == 0)
                        for (const auto& spec : kSpecs) known = known || probe.substr(std::string(slot).size()) == spec.suffix;
                if (!known) return {false, "unknown setting " + it.key()};
                next.erase(it.key());
                continue;
            }
            const auto problem = checkPumpValue(it.key(), it.value());
            if (!problem.empty()) return {false, problem};
            next[it.key()] = it.value();
        }
        const auto saved = writeLocked(next);
        if (!saved.ok) return saved;
    }
    applyReservation();
    return {true, "Pump settings saved"};
}

HostSettingsStore::Result HostSettingsStore::setBootDisabledServices(const std::vector<std::string>& tokens) {
    Json list = Json::array();
    for (const auto& raw : tokens) {
        const auto token = normaliseToken(raw);
        if (token.empty()) continue;
        const auto& known = knownServiceTokens();
        if (std::find(known.begin(), known.end(), token) == known.end()) return {false, "Unknown service " + raw};
        if (std::find(list.begin(), list.end(), token) == list.end()) list.push_back(token);
    }
    std::sort(list.begin(), list.end());
    std::lock_guard<std::mutex> lock(mutex_);
    Json next = doc_;
    if (list.empty()) next.erase(kBootKey); else next[kBootKey] = list;
    const auto saved = writeLocked(next);
    if (!saved.ok) return saved;
    return {true, "Boot service toggles saved. Restart the application for the change to take effect."};
}

std::string HostSettingsStore::bootDisabledServicesCsv() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = doc_.find(kBootKey);
    std::string csv;
    if (it == doc_.end() || !it->is_array()) return csv;
    for (const auto& token : *it) {
        if (!csv.empty()) csv += ',';
        csv += token.get<std::string>();
    }
    return csv;
}

std::vector<std::string> HostSettingsStore::pumpPorts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ports;
    for (const char* slot : kSlots) {
        const std::string prefix(slot);
        const auto text = doc_.find(prefix + "port");
        if (text != doc_.end() && text->is_string()) { ports.push_back(text->get<std::string>()); continue; }
        const auto number = doc_.find(prefix + "com_port");
        if (number != doc_.end() && number->is_number_integer() && number->get<int>() > 0)
            ports.push_back("COM" + std::to_string(number->get<int>()));
    }
    return ports;
}

void HostSettingsStore::applyReservation() const { claims::setReserved(kReservationOwner, pumpPorts()); }
void HostSettingsStore::clearReservation() { claims::setReserved(kReservationOwner, {}); }

const std::vector<std::string>& HostSettingsStore::knownServiceTokens() {
    static const std::vector<std::string> tokens{"sqlite", "hdf5",     "processing", "autofocus", "trigger",
                                                 "capture", "playback", "dot_grid",   "auto_update", "all"};
    return tokens;
}

} // namespace backend::app
