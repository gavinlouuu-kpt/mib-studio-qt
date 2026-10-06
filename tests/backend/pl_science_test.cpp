// Science on the PL (YOFO Studio ADR 0008, MIB_PL_SCIENCE): the PS build never runs the host
// processing pipeline. Exercised on x86 through the environment switch the policy also honours.
#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/app/SciencePlacement.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/tempdir.h"

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>

namespace {
// POSIX setenv is not available with MSVC.
void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
}  // namespace

int main()
{
    setEnv("MIB_PL_SCIENCE", "1");
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_DISABLED_SERVICES", "sqlite,hdf5,yolo,autofocus,trigger,playback");
    MIB_REQUIRE(!backend::app::hostProcessingAvailable(), "MIB_PL_SCIENCE=1 places the science on the PL");
    MIB_EXPECT(std::string(backend::app::sciencePlacement()) == "pl", "placement reads pl");

    mib::test::TempDir temp("pl_science");
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(temp.path().string()), "facade initializes");
    const auto info = nlohmann::json::parse(facade.fetchPlatformInfoJson());
    MIB_EXPECT(info["science"] == "pl" && info["host_processing"] == false, "platform info reports the PL");
    // #501: the PZ7035 surfaces, and none of the MIB-only ones.
    const auto& caps = info["capabilities"];
    MIB_EXPECT(caps["instrument"] == "pz7035" && caps["pl_identity"] == true && caps["led_strobe"] == true,
               "capabilities name the PZ7035");
    MIB_EXPECT(caps["autofocus"] == false && caps["trigger"] == false && caps["host_background"] == false &&
                   caps["frame_buffer"] == false && caps["reanalysis"] == false && caps["core_updates"] == false &&
                   caps["egrabber_script"] == false,
               "no MIB-only surfaces on the PZ7035");
    MIB_EXPECT(caps["pump"]["model"] == "tushui_peristaltic" && caps["pump"]["port"] == "/dev/ttyPS1" &&
                   caps["pump"]["sample_address"] == 3 && caps["pump"]["sheath_address"] == 4,
               "the peristaltic pumps on ttyPS1, slaves 3 and 4");
    // No board provider here (MIB_EXECUTION_PROVIDER unset): status says why.
    const auto status = nlohmann::json::parse(facade.fetchInstrumentStatusJson());
    MIB_EXPECT(status["available"] == false &&
                   status["error"].get<std::string>().find("MIB_EXECUTION_PROVIDER=pz") != std::string::npos,
               "instrument status without the board provider explains itself");

    backend::bridge::ProcessingSettingsCommand on;
    on.realtimeEnabled = true;
    const auto refused = facade.dispatch(on);
    MIB_EXPECT(!refused.ok && refused.message.find("PL") != std::string::npos,
               "enabling realtime processing is refused with the reason");
    backend::bridge::ProcessingSettingsCommand off;
    off.realtimeEnabled = false;
    off.pixelToMicronFactor = 0.5;
    MIB_EXPECT(facade.dispatch(off).ok, "settings without the host pipeline still apply");

    backend.processing().setRealtimeEnabled(true);
    MIB_EXPECT(!backend.processing().isRealtimeEnabled(), "the service itself refuses the switch");
    backend.processing().startRealtime(backend.getFrameStore());
    MIB_EXPECT(!backend.processing().isRealtimeRunning(), "startRealtime is a no-op");

    const auto readiness = backend.experiment().evaluateReadiness(temp.path().string());
    bool pl = false, host = false;
    for (const auto& gate : readiness.gates) {
        if (gate.id == "science.pl") pl = true;
        if (gate.id.rfind("processing.", 0) == 0) host = true;
    }
    MIB_EXPECT(pl && !host, "readiness carries the PL gate and none of the host pipeline's");
    facade.shutdown();
    return mib::test::exitCode();
}
