// Instrument-host settings the desktop app keeps beside the data directory (epic #868, HW-003/016/023/038).
//
// The Qt app keeps the syringe pump connection (`pump_*_com_port`, `_baud_rate`, `_address`, `_syringe_vol`,
// `_syringe_unit`) and the last rate, unit and direction (`pump_*_flow_rate`, `_flow_unit`, `_direction`) in its
// config.json, and the boot service toggles in QSettings (`Startup/DisabledServices`). The Tauri host has no such
// live config.json (profiles and methods are read-only, hash-checked documents), so this store holds the same keys
// in `<data>/host_settings.json`, loaded before the backend starts:
//  - the saved pump ports are reserved at once (SerialPortClaims `setReserved`), so the startup nanopositioner
//    probe and the generator sweep never open a pump bus, before any client has connected;
//  - the boot toggles become the backend's service list (`AppBackend::setBootDisabledServices`), with
//    MIB_DISABLED_SERVICES still winning when it is set, as in Qt's main.cpp.
// Vault: knowledge_map/services/HostSettings.md.
#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace backend::app {

class HostSettingsStore {
public:
    struct Result {
        bool ok{false};
        std::string message;
    };

    // `path` empty = memory only (no data directory): reads work, writes are refused.
    explicit HostSettingsStore(std::string path);

    // Reads the file. A missing file is empty settings; an unreadable or invalid one is kept out (empty settings,
    // `loadError()` says why) and never overwritten until the operator saves something valid.
    void load();
    std::string loadError() const;
    std::string path() const;

    // The stored document, keys sorted; only the keys this store knows.
    nlohmann::json document() const;

    // Merge-patch of the pump keys. Every key is checked first; one bad value refuses the whole patch (atomic) and
    // names the key. `null` removes a key. The file is replaced atomically (temp file + rename).
    Result setPumps(const nlohmann::json& patch);

    // The services to leave off at the next start (Qt's Startup/DisabledServices tokens). Tokens are normalised like
    // Qt (trim, lower case, '-' to '_') and must be known; an empty list clears the setting.
    Result setBootDisabledServices(const std::vector<std::string>& tokens);
    std::string bootDisabledServicesCsv() const;

    // The pump ports to keep probes off: `pump_<slot>_port` when saved, else `COM<pump_<slot>_com_port>`.
    std::vector<std::string> pumpPorts() const;

    // Applies pumpPorts() to the process-wide port claims (owner "the syringe pumps (saved pump settings)").
    void applyReservation() const;
    static void clearReservation();

    static const std::vector<std::string>& knownServiceTokens();
    static constexpr const char* kReservationOwner = "the syringe pumps (saved pump settings)";

private:
    Result writeLocked(const nlohmann::json& next);

    mutable std::mutex mutex_;
    std::string path_;
    std::string loadError_;
    nlohmann::json doc_ = nlohmann::json::object();
};

} // namespace backend::app
