// Scenario files and the environment switch for simulated serial hardware
// (SimulatedSerial.h). Lives in mib_backend: it needs nlohmann::json, which
// the oeabt_serial archive does not link.
#pragma once

#include "backend/services/SimulatedSerial.h"

#include <memory>
#include <optional>
#include <string>

namespace backend::services::serialsim {

// Build a rig from scenario JSON (format: docs/howto/simulated-serial.md).
// `seedOverride` replaces the file's seed. Throws std::runtime_error naming
// the offending field.
std::shared_ptr<Rig> loadScenario(const std::string& jsonText,
                                  std::optional<uint64_t> seedOverride = std::nullopt);

// MIB_SERIAL_MODE=sim installs a rig for the whole process:
//   MIB_SERIAL_SIM_SCENARIO  scenario JSON path (default: makeBenchRig)
//   MIB_SERIAL_SIM_SEED      jitter seed (default: the scenario's, else 1)
// Returns true when a rig is installed. A scenario that cannot be read or
// parsed installs nothing: the app must never fall back to real ports while
// the operator believes they are simulated, so the caller refuses to start.
// `error` explains why.
bool installFromEnvironment(std::string& error);

} // namespace backend::services::serialsim
