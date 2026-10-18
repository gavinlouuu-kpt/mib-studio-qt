#pragma once

// Run configuration snapshot v3 (#712, ADR 0015 item 5): what a run records about itself beyond
// the v2 snapshot, as plain values shared by the writer (ExperimentCoordinator, runSnapshotToJson)
// and the typed reader (export, Review, tooling).
//
// - the run's own id (a UUID minted at Start; the FCS $MIB_RUN_ID);
// - the E-modulus LUT identity: the SHA-256 of the bytes that were loaded, plus the catalog's
//   labels for them;
// - device facts at Start: pump setpoints and the last complete status poll, the PZ7035 LED strobe
//   registers, the Z stage's last status and the piezo's last known voltage;
// - the run id armed into the PZ7035 PL (its RUN_ID registers);
// - the stop record (/run_provenance @run_stop_json), written at finalization.
//
// Unknown is std::nullopt, never 0, false or "": an absent device or a fact the device did not
// report is written as absent (null or a missing key) and reads back as nullopt. An empty string is
// never a value: it is written as null and read as nullopt. Files that predate v3 read with every
// member nullopt. Nothing here is fabricated from a default.
// Qt-free; part of mib_processing.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend::services {
class Hdf5Service;
}

namespace backend::recording {

// The run_snapshot_schema_version this code writes (RunConfigurationSnapshot::kSchemaVersion).
inline constexpr uint32_t kRunSnapshotSchemaVersion = 3;

// The E-modulus LUT a run used. `sha256` is the identity (computed from the bytes that were
// loaded); lutId / revision / source / catalogSha256 are what the LUT catalog says about the file
// and describe the loaded bytes only when catalogSha256 equals sha256 (the catalog can report a
// manifest whose download failed while the older cached file stays loaded).
struct EModulusLutRecord {
    bool loaded{false};                         // no LUT: E values and the E gate were unavailable
    std::optional<std::string> lutId;
    std::optional<std::string> revision;
    std::optional<std::string> source;          // "r2-public-catalog" | "local-cache" | "bundled-fallback"
    std::optional<std::string> sha256;          // of the loaded bytes
    std::optional<std::string> catalogSha256;   // the catalog's declared hash
    std::optional<std::string> checksumStatus;  // the catalog's "verified" | "unknown"
    // sha256 == catalogSha256 (case-insensitive); nullopt when either is unknown.
    std::optional<bool> catalogMatchesLoaded() const;
};

// The last complete status poll of a pump (all registers of one poll answered).
struct PumpReadbackRecord {
    uint64_t ageUs{0};                         // how long before the snapshot it was read
    std::optional<bool> lastPollOk;            // false: a newer poll did not answer completely
    std::string runStatus;                     // "stop" | "forward" | "backward" | "pause" (required)
    std::optional<double> speedRpm;            // peristaltic: head speed register (rpm)
    std::optional<double> realtimeFlowRaw;     // syringe: realtime flow register, device units
    std::optional<double> accumulatedVolumeRaw; // syringe: accumulated volume register, device units
    std::optional<bool> stalled;               // syringe: error status bit 3
};

struct PumpRecord {
    std::string slot;                          // "sample" | "sheath"
    // nullopt: the pump was busy with serial I/O at Start and was not sampled (unknown).
    std::optional<bool> connected;
    // Present only when connected.
    std::optional<std::string> model;          // "dlsp_syringe" | "tushui_peristaltic"
    std::optional<std::string> port;
    std::optional<int> modbusAddress;
    // Setpoint: the settings the pump is known to hold (written successfully or read back in this
    // connection, SyringePumpService::AppliedSettings); each nullopt until then.
    std::optional<double> flowRate;
    std::optional<int> flowRateUnitCode;       // 100 = µL/min, 103 = mL/min
    std::optional<std::string> flowRateUnit;   // "uL/min" | "mL/min"; nullopt for another code
    std::optional<std::string> direction;      // "infuse" | "withdraw"
    std::optional<double> microlitersPerRev;   // peristaltic flow calibration
    // nullopt: no complete status poll since connect.
    std::optional<PumpReadbackRecord> readback;
};

// The PZ7035 LED strobe registers as read back at Start (S[0] control, S[1] delay, S[2] width in
// strobe-clock cycles, S[10] strobe clock).
struct LedRecord {
    std::optional<std::string> source;         // "pz7035_strobe"
    std::optional<std::string> error;          // why the registers could not be read
    std::optional<uint32_t> control;           // 0 off, 1 XVS sync
    std::optional<double> delayUs;
    std::optional<double> widthUs;
    std::optional<uint32_t> clockKhz;
    // The Start left a stopped Run: it switches the LED on with the Run preset after this readback.
    std::optional<bool> stoppedRunAtStart;
    std::optional<bool> enabled() const;
};

struct StageReadbackRecord {
    uint64_t ageUs{0};                         // how long before the snapshot the status was sampled
    // Each nullopt when the readback did not carry it (a missing alarm flag is unknown, not "no alarm").
    std::optional<std::string> state;          // "idle" | "moving" | "homing" | "faulted"
    std::optional<double> positionUm;          // relative to the operator's zero (see StageRecord::zeroSet)
    std::optional<bool> limitPositive;
    std::optional<bool> limitNegative;
    std::optional<bool> home;
    std::optional<bool> emergencyStop;
    std::optional<bool> driverAlarm;
};

struct StageRecord {
    bool connected{false};
    std::optional<std::string> model, serial, firmware;
    std::optional<bool> zeroSet;               // false: positionUm has no operator zero
    std::optional<StageReadbackRecord> readback;
};

// The nanopositioner (piezo). `voltageV` is the service's last value: read at connect or by the
// autofocus loop, or the last voltage written. No fresh read is taken at Start.
struct PiezoRecord {
    bool connected{false};
    std::optional<double> voltageV;
    std::optional<bool> autofocusEnabled;
};

// The members snapshot v3 adds to run_snapshot_json.
struct RunProvenanceV3 {
    std::optional<std::string> runId;          // UUID v4, lowercase
    std::optional<EModulusLutRecord> eModulusLut;
    std::optional<std::vector<PumpRecord>> pumps;
    std::optional<LedRecord> led;              // nullopt: this instrument has no LED readback
    std::optional<StageRecord> stage;
    std::optional<PiezoRecord> piezo;
    std::optional<uint64_t> plRunId;           // RUN_ID armed into the PL; nullopt for a host run
};

// How the run ended, written at finalization. `reason` names the coordinator path:
// "stop_requested" (every requestStop(false) caller: the UI's Stop and the bridge server's
// client-loss stop alike), "cancel_requested", "fatal_save_error", "shutdown".
struct RunStopRecord {
    std::string reason;                        // required
    std::optional<std::string> message;        // the failure message; nullopt when there is none
};

// The typed view of a file's run provenance.
struct RecordedRunProvenance {
    uint32_t schemaVersion{0};                 // run_snapshot_json "schema_version"; 0: no snapshot
    RunProvenanceV3 v3;                        // every member nullopt before v3
    std::optional<RunStopRecord> stop;         // nullopt: no stop record (older file, or unreadable)
};

// The v3 members as JSON object members, in a stable order and without braces or a leading comma:
// `"run_id":...,"emodulus_lut":...,"pumps":...,"led":...,"stage":...,"piezo":...,"pl_run_id":...`.
std::string runProvenanceV3Members(const RunProvenanceV3& v3);

// Parse a run_snapshot_json document. Members it lacks, or holds as null, stay nullopt; a member
// of the wrong type is treated as absent. False (with `why`) only when the text is not a JSON
// object.
bool parseRunProvenance(const std::string& runSnapshotJson, RecordedRunProvenance& out,
                        std::string* why = nullptr);

std::string runStopToJson(const RunStopRecord& stop);
std::optional<RunStopRecord> parseRunStop(const std::string& json, std::string* why = nullptr);

// Read and parse /run_provenance of an open file. False when the file has no readable snapshot
// (out.schemaVersion stays 0); the stop record is read independently of the snapshot.
bool readRecordedRunProvenance(const services::Hdf5Service& file, RecordedRunProvenance& out,
                               std::string* why = nullptr);

} // namespace backend::recording
