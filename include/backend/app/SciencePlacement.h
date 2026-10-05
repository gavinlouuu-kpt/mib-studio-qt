// Where per-frame science runs (YOFO Studio ADR 0008).
//
// On the PZ7035 the PL processes every frame: mask, components, metrics, decision. The PS only
// moves previews to the screen and records to disk, so the desktop processing pipeline
// (ProcessingService realtime, backgrounds, host ROI) must never run there: it would burn the A9
// on work the PL already did and produce a second, wrong set of results.
#pragma once

namespace backend::app {

// False when built with MIB_PL_SCIENCE (the linux-armv7-yocto preset) or when MIB_PL_SCIENCE=1
// is in the environment (tests on x86). Evaluated once.
bool hostProcessingAvailable();

// "host" or "pl", for the UI and the readiness gates.
const char* sciencePlacement();

} // namespace backend::app
