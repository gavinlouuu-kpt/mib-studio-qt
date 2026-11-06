// host_settings_test
//
// The instrument-host settings the desktop app keeps (HostSettings.h; epic #868, HW-003, HW-016, HW-020, HW-021,
// HW-023, HW-038):
//  1. the store: Qt's pump keys are validated as a whole (one bad value refuses the patch and leaves the file as it
//     was), `null` removes a key, a damaged file is left alone until a valid save, boot toggles are normalised;
//  2. the reservation: the saved pump ports (an integer COM number or a system path) are reserved from the store
//     alone, whatever the client does;
//  3. through the facade on the simulated bench (COM3 pumps, COM5 nanopositioner): with the pump port saved the
//     startup nanopositioner probe never writes onto the pump bus (no ForeignFrame on COM3) and still finds the
//     controller; without it the probe does reach COM3 (the "before" of the same run); the address scan finds both
//     pumps, reports a port that will not open, and the boot toggles reach the next start.

#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/HostSettings.h"
#include "backend/discovery/StartupDiscoveryCoordinator.h"
#include "backend/services/SerialPortClaims.h"
#include "backend/services/SimulatedSerial.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace serialsim = backend::services::serialsim;
namespace claims = backend::services::portclaims;
using backend::app::HostSettingsStore;
using backend::discovery::StartupDiscoveryCoordinator;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}
void spit(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void storeRules() {
    mib::test::TempDir dir("host_settings_store");
    const auto file = (dir.path() / "host_settings.json").string();

    HostSettingsStore store(file);
    store.load();
    MIB_EXPECT(store.document().empty() && store.loadError().empty(), "a missing file is empty settings, not an error");
    MIB_EXPECT(store.pumpPorts().empty(), "nothing reserved");

    // Qt's keys, as the dialog and the tab write them.
    const auto saved = store.setPumps(Json::parse(R"({
        "pump_sample_com_port": 3, "pump_sample_port": "COM3", "pump_sample_baud_rate": 115200, "pump_sample_address": 1,
        "pump_sample_syringe_vol": 0.5, "pump_sample_syringe_unit": 103,
        "pump_sample_flow_rate": 20.5, "pump_sample_flow_unit": 100, "pump_sample_direction": 1,
        "pump_sheath_port": "/dev/ttyUSB0", "pump_sheath_baud_rate": 9600, "pump_sheath_address": 2})"));
    MIB_REQUIRE(saved.ok, saved.message);
    const auto before = slurp(file);
    MIB_EXPECT(Json::parse(before).value("version", 0) == 1, "the file carries its version");

    HostSettingsStore again(file);
    again.load();
    MIB_EXPECT(again.document() == store.document(), "a second store reads what the first wrote");
    MIB_EXPECT(again.document().at("pump_sample_flow_rate") == 20.5 && again.document().at("pump_sheath_port") == "/dev/ttyUSB0", "values round-trip");

    // One bad value refuses the whole patch and leaves memory and file as they were.
    for (const char* bad : {R"({"pump_sample_address": 248})", R"({"pump_sample_address": 1.5})", R"({"pump_sample_baud_rate": "115200"})",
                            R"({"pump_sample_syringe_unit": 101})", R"({"pump_sample_flow_unit": 7})", R"({"pump_sample_direction": 2})",
                            R"({"pump_sample_syringe_vol": 0})", R"({"pump_sample_flow_rate": -1})", R"({"pump_sample_port": ""})",
                            R"({"pump_sample_port": 3})", R"({"pump_other_address": 1})", R"({"unknown": 1})", R"({"boot_disabled_services": []})",
                            R"({"pump_sample_address": 2, "pump_sheath_address": 0})", R"({"pump_sample_address": null, "pump_nothing": null})", "[]", "{}"}) {
        const auto refused = store.setPumps(Json::parse(bad));
        MIB_EXPECT(!refused.ok && !refused.message.empty(), std::string("refused: ") + bad);
    }
    MIB_EXPECT(slurp(file) == before && store.document() == again.document(), "a refused patch changes nothing, in memory or on disk");

    // Rate persistence merges: the connection survives, and `null` removes a key.
    MIB_REQUIRE(store.setPumps(Json::parse(R"({"pump_sample_flow_rate": 30, "pump_sample_flow_unit": 200})")).ok, "rate merge");
    MIB_EXPECT(store.document().at("pump_sample_port") == "COM3" && store.document().at("pump_sample_flow_rate") == 30, "merge keeps the other keys");
    MIB_REQUIRE(store.setPumps(Json::parse(R"({"pump_sample_com_port": null, "pump_sample_port": null})")).ok, "remove");
    MIB_EXPECT(!store.document().contains("pump_sample_com_port") && !store.document().contains("pump_sample_port"), "null removes");

    // Reservation: the saved text first, else COM<n>; system names key like the bus layer does.
    MIB_REQUIRE(store.setPumps(Json::parse(R"({"pump_sample_com_port": 7})")).ok, "com port only");
    MIB_EXPECT((store.pumpPorts() == std::vector<std::string>{"COM7", "/dev/ttyUSB0"}), "COM number or system path");
    store.applyReservation();
    MIB_EXPECT(claims::isReserved("COM7") && claims::isReserved("com7") && claims::isReserved("\\\\.\\COM7"), "COM7 reserved");
    MIB_EXPECT(claims::isReserved("/dev/ttyUSB0") && claims::isReserved("ttyUSB0"), "a system path is reserved too");
    MIB_EXPECT(!claims::isReserved("COM8"), "only the saved ports");
    MIB_REQUIRE(store.setPumps(Json::parse(R"({"pump_sheath_port": null})")).ok, "drop sheath");
    MIB_EXPECT(!claims::isReserved("ttyUSB0") && claims::isReserved("COM7"), "a save replaces the reservation");
    std::string owner;
    MIB_EXPECT(claims::isReserved("COM7", &owner) && owner == HostSettingsStore::kReservationOwner, "named for the operator: " + owner);
    HostSettingsStore::clearReservation();
    MIB_EXPECT(!claims::isReserved("COM7"), "cleared");

    // Boot toggles: Qt's normalisation, known tokens only, sorted, empty clears.
    MIB_EXPECT(!store.setBootDisabledServices({"trigger", "bogus"}).ok && store.bootDisabledServicesCsv().empty(), "unknown service refused");
    MIB_REQUIRE(store.setBootDisabledServices({" Trigger ", "dot-grid", "autofocus", "trigger"}).ok, "boot toggles");
    MIB_EXPECT(store.bootDisabledServicesCsv() == "autofocus,dot_grid,trigger", "normalised, de-duplicated, sorted: " + store.bootDisabledServicesCsv());
    MIB_EXPECT(store.document().at("pump_sample_com_port") == 7, "boot toggles leave the pump keys alone");
    HostSettingsStore reread(file);
    reread.load();
    MIB_EXPECT(reread.bootDisabledServicesCsv() == "autofocus,dot_grid,trigger", "boot toggles survive a restart");
    MIB_REQUIRE(store.setBootDisabledServices({}).ok, "clear");
    MIB_EXPECT(store.bootDisabledServicesCsv().empty() && !store.document().contains("boot_disabled_services"), "empty clears");

    // A damaged file is left alone until the operator saves something valid; a hand-edited one keeps what is valid.
    spit(file, "{not json");
    HostSettingsStore damaged(file);
    damaged.load();
    MIB_EXPECT(damaged.document().empty() && !damaged.loadError().empty(), "damaged file reported");
    MIB_EXPECT(slurp(file) == "{not json", "and not rewritten by loading it");
    MIB_REQUIRE(damaged.setPumps(Json::parse(R"({"pump_sheath_address": 4})")).ok, "a valid save replaces it");
    MIB_EXPECT(damaged.loadError().empty() && Json::parse(slurp(file)).at("pump_sheath_address") == 4, "replaced");
    spit(file, R"({"pump_sample_address": 9, "pump_sample_baud_rate": "fast", "mystery": 1, "boot_disabled_services": ["Trigger", 5, "trigger"]})");
    HostSettingsStore partial(file);
    partial.load();
    MIB_EXPECT(partial.document().size() == 2 && partial.document().at("pump_sample_address") == 9 && partial.bootDisabledServicesCsv() == "trigger",
               "valid keys are kept, the rest is left out: " + partial.document().dump());

    // No data directory: reads work, writes say why.
    HostSettingsStore memory{std::string{}};
    memory.load();
    MIB_EXPECT(!memory.setPumps(Json::parse(R"({"pump_sample_address": 1})")).ok, "no data directory, no save");
}

struct StartupRun {
    std::optional<StartupDiscoveryCoordinator::NanopositionerOutcome::Kind> outcome;
    int foreignFramesOnPumpBus{0};
    int foreignFramesOnPumpBusAtOutcome{0};
    std::string summary;
};

// One startup nanopositioner search on the simulated bench, with `hostSettings` (may be empty) in the data directory.
StartupRun startupSearch(const std::string& hostSettings, std::uint64_t seed) {
    StartupRun run;
    auto rig = serialsim::makeBenchRig(seed);
    serialsim::install(rig);
    {
        mib::test::TempDir dir("host_settings_startup");
        if (!hostSettings.empty()) spit(dir.path() / "host_settings.json", hostSettings);
        backend::AppBackend backend;
        backend::bridge::BackendFacade facade(backend);
        MIB_REQUIRE(facade.initialize(dir.path().string()), "facade initializes");

        StartupDiscoveryCoordinator::Timing timing;
        timing.cameraDelay = 20ms;
        timing.nanopositionerRetries = 6;
        timing.nanopositionerRetryDelay = 300ms;
        timing.nanopositionerDeadline = 15000ms;
        backend.startupDiscovery().setTiming(timing);

        std::mutex m;
        std::condition_variable cv;
        backend.startupDiscovery().setNanopositionerListener([&](const StartupDiscoveryCoordinator::NanopositionerOutcome& o) {
            using Kind = StartupDiscoveryCoordinator::NanopositionerOutcome::Kind;
            if (o.kind == Kind::Started || o.kind == Kind::Searching) return;
            std::lock_guard<std::mutex> lock(m);
            run.outcome = o.kind;
            cv.notify_all();
        });
        // The operator's Refresh: a nanopositioner search over the bench's adapters.
        MIB_REQUIRE(backend.startupDiscovery().runNanopositionerStep(), "the search is accepted");
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, 20s, [&] { return run.outcome.has_value(); });
        }
        facade.shutdown();
    }
    for (const auto& e : rig->events(serialsim::Event::Kind::ForeignFrame)) run.foreignFramesOnPumpBus += e.port == "COM3";
    run.summary = rig->summary();
    serialsim::install(nullptr);
    return run;
}

void reservationKeepsTheProbeOffThePumpBus() {
    using Kind = StartupDiscoveryCoordinator::NanopositionerOutcome::Kind;
    // Before: no pump port saved, so the probe may identify COM3 (the pumps' CH340 adapter) by writing an ASCII identity
    // command onto the RS485 bus. This is what the operator's Refresh did on the simulated bench.
    const auto unreserved = startupSearch("", 5);
    MIB_EXPECT(unreserved.outcome && *unreserved.outcome == Kind::Connected, "the controller is found without a saved pump port");
    MIB_EXPECT(unreserved.foreignFramesOnPumpBus > 0, "baseline: without a saved pump port the probe writes onto COM3\n" + unreserved.summary);

    // After: the same search with the pump port saved, as Qt's config.json names it. Nothing reaches COM3.
    for (const char* saved : {R"({"pump_sample_com_port":3,"pump_sheath_com_port":3})",
                              R"({"pump_sample_port":"COM3","pump_sheath_port":"COM3"})"}) {
        const auto reserved = startupSearch(saved, 5);
        MIB_EXPECT(reserved.outcome && *reserved.outcome == Kind::Connected, std::string("the controller is still found: ") + saved);
        MIB_EXPECT(reserved.foreignFramesOnPumpBus == 0, std::string("no ForeignFrame on the pump bus with ") + saved + "\n" + reserved.summary);
    }
}

void scanAndSettingsThroughTheFacade() {
    auto rig = serialsim::makeBenchRig(9);
    serialsim::install(rig);
    {
        mib::test::TempDir dir("host_settings_facade");
        backend::AppBackend backend;
        backend::bridge::BackendFacade facade(backend);
        MIB_REQUIRE(facade.initialize(dir.path().string()), "facade initializes");
        const auto settings = [&] { return Json::parse(facade.fetchHostSettingsJson()); };
        std::this_thread::sleep_for(500ms); // the bench's pump adapter (COM3) enumerates 300 ms after the rig is installed
        MIB_EXPECT(settings().at("valid") == true && settings().at("settings").empty() && settings().at("pump_scan").at("state") == "idle", "fresh");

        const auto waitScan = [&] {
            for (int i = 0; i < 200; ++i) {
                if (settings().at("pump_scan").at("state") != "running") break;
                std::this_thread::sleep_for(50ms);
            }
            return settings().at("pump_scan");
        };

        // Scan 1-8 at 300 ms on the pumps' bus: both simulated pumps answer.
        MIB_EXPECT(!facade.hostSettingsCommandJson(R"({"action":"scan_pump_addresses","port":"","baud":115200})").ok, "no port, no scan");
        MIB_EXPECT(!facade.hostSettingsCommandJson(R"({"action":"scan_pump_addresses","port":"COM3","baud":115200,"start":5,"end":2})").ok, "reversed range");
        MIB_EXPECT(!facade.hostSettingsCommandJson(R"({"action":"scan_pump_addresses","port":"COM3","baud":"fast"})").ok, "baud must be a number");
        const auto started = facade.hostSettingsCommandJson(R"({"action":"scan_pump_addresses","port":"COM3","baud":115200,"start":1,"end":8,"timeout_ms":300})");
        MIB_REQUIRE(started.ok && started.operationId != 0, started.message);
        const auto during = settings().at("pump_scan");
        MIB_EXPECT(during.at("port") == "COM3" && during.at("operation_id") == std::to_string(started.operationId), "the scan is named");
        const auto second = facade.hostSettingsCommandJson(R"({"action":"scan_pump_addresses","port":"COM3","baud":115200})");
        MIB_EXPECT(during.at("state") != "running" || !second.ok, "a second scan waits for the first");
        const auto done = waitScan();
        MIB_EXPECT(done.at("state") == "completed" && done.at("error") == "" && done.at("found") == Json::array({1, 2}), "Found 2: " + done.dump());

        // A port that is not there says why instead of "no pump answered".
        MIB_REQUIRE(facade.hostSettingsCommandJson(R"({"action":"scan_pump_addresses","port":"COM9","baud":115200,"start":1,"end":2,"timeout_ms":50})").ok, "scan COM9 starts");
        const auto missing = waitScan();
        MIB_EXPECT(missing.at("state") == "failed" && !missing.at("error").get<std::string>().empty() && missing.at("found").empty(), "COM9: " + missing.dump());

        // Review of #890: a port an exclusive service holds (the connected nanopositioner) is refused, before any open.
        {
            auto held = claims::claimForService("COM5", "autofocus test");
            const auto refused = facade.hostSettingsCommandJson(R"({"action":"scan_pump_addresses","port":"COM5","baud":115200,"start":1,"end":2,"timeout_ms":50})");
            MIB_EXPECT(!refused.ok && refused.message.find("in use by service:autofocus test") != std::string::npos, "held port refused: " + refused.message);
            MIB_EXPECT(settings().at("pump_scan").at("port") == "COM9", "a refused scan leaves the last scan as it was");
        }

        // Pump settings through the command: validated whole, reserved at once, readable back.
        MIB_EXPECT(!facade.hostSettingsCommandJson(R"({"action":"set_pumps","settings":{"pump_sample_address":0}})").ok, "range refused");
        MIB_EXPECT(!facade.hostSettingsCommandJson(R"({"action":"set_pumps"})").ok && !facade.hostSettingsCommandJson("not json").ok &&
                   !facade.hostSettingsCommandJson(R"({"action":"bogus"})").ok, "malformed requests refused");
        MIB_EXPECT(!claims::isReserved("COM3"), "nothing reserved before a save");
        MIB_REQUIRE(facade.hostSettingsCommandJson(R"({"action":"set_pumps","settings":{"pump_sample_com_port":3,"pump_sample_port":"COM3","pump_sample_baud_rate":115200,"pump_sample_address":1}})").ok, "save");
        MIB_EXPECT(claims::isReserved("COM3"), "a save reserves the port at once");
        MIB_EXPECT(settings().at("settings").at("pump_sample_address") == 1 && settings().at("path").get<std::string>().find("host_settings.json") != std::string::npos, "read back");

        // Boot toggles: saved, read back, and the next facade on the same directory starts with them.
        MIB_REQUIRE(facade.hostSettingsCommandJson(R"({"action":"set_boot_services","services":["Trigger","autofocus"]})").ok, "boot save");
        MIB_EXPECT(!facade.hostSettingsCommandJson(R"({"action":"set_boot_services","services":["nope"]})").ok, "unknown service refused");
        MIB_EXPECT(settings().at("settings").at("boot_disabled_services") == Json::array({"autofocus", "trigger"}), "boot toggles read back");
        facade.shutdown();
        MIB_EXPECT(!claims::isReserved("COM3"), "shutdown releases the reservation");

        backend::AppBackend next;
        backend::bridge::BackendFacade second2(next);
        MIB_REQUIRE(second2.initialize(dir.path().string()), "second start");
        MIB_EXPECT(claims::isReserved("COM3"), "the saved port is reserved again at the next start, before any client");
        MIB_EXPECT(Json::parse(second2.fetchHostSettingsJson()).at("settings").at("pump_sample_address") == 1, "and the settings are back");
        second2.shutdown();
    }
    serialsim::install(nullptr);
}

void environmentWinsOverSavedBootToggles() {
    // Qt's main.cpp: MIB_DISABLED_SERVICES in the environment is respected first. The saved list is used otherwise.
    // AppBackend logs the list it used; the observable part here is that the backend still starts with either.
    for (const char* env : {"", "trigger"}) {
        setEnv("MIB_DISABLED_SERVICES", env);
        mib::test::TempDir dir("host_settings_boot");
        spit(dir.path() / "host_settings.json", R"({"boot_disabled_services":["autofocus","trigger"]})");
        backend::AppBackend backend;
        backend::bridge::BackendFacade facade(backend);
        MIB_EXPECT(facade.initialize(dir.path().string()), std::string("starts with MIB_DISABLED_SERVICES='") + env + "'");
        MIB_EXPECT(Json::parse(facade.fetchHostSettingsJson()).at("boot_env") == env, "the environment is reported");
        facade.shutdown();
    }
    setEnv("MIB_DISABLED_SERVICES", nullptr);
}

} // namespace

int main() {
    storeRules();
    reservationKeepsTheProbeOffThePumpBus();
    scanAndSettingsThroughTheFacade();
    environmentWinsOverSavedBootToggles();
    std::printf("host_settings: %s\n", mib::test::exitCode() == 0 ? "all checks passed" : "FAILED");
    return mib::test::exitCode();
}
