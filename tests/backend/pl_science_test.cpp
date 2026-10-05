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

int main()
{
    setenv("MIB_PL_SCIENCE", "1", 1);
    setenv("MIB_CAMERA_MODE", "mock", 1);
    setenv("MIB_DISABLED_SERVICES", "sqlite,hdf5,yolo,autofocus,trigger,playback", 1);
    MIB_REQUIRE(!backend::app::hostProcessingAvailable(), "MIB_PL_SCIENCE=1 places the science on the PL");
    MIB_EXPECT(std::string(backend::app::sciencePlacement()) == "pl", "placement reads pl");

    mib::test::TempDir temp("pl_science");
    backend::AppBackend backend;
    backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(temp.path().string()), "facade initializes");
    const auto info = nlohmann::json::parse(facade.fetchPlatformInfoJson());
    MIB_EXPECT(info["science"] == "pl" && info["host_processing"] == false, "platform info reports the PL");

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
