// What the backend re-applies at startup (#398 M2c): the last applied local
// profile or central method, whichever came last. One pointer, so the two
// cannot both claim to be in effect after a restart.
//
// <dataDir>/startup_configuration.json holds either
//   {"kind":"profile","base":<profiles folder>,"name":..,"revision":..}
//   {"kind":"central","revision_id":..,"config_sha256":..}
// Both apply paths record it after a successful apply (ProfileStore's
// "apply" and applyCentralMethod). A pointer write failure is logged, never
// undoes the applied settings.
#pragma once

#include <string>

namespace backend {
class AppBackend;
}

namespace backend::app {

void recordStartupProfile(AppBackend& backend, const std::string& base, const std::string& name,
                          const std::string& revision);
void recordStartupCentralMethod(AppBackend& backend, const std::string& revisionId,
                                const std::string& configSha256);

// Re-applies what the pointer names, through the shared validator, and
// returns JSON {ok, restored, kind ("profile"|"central"|null), error?, ...}.
//  - profile: the profile store's "restore" in the recorded folder (refused
//    if the profile changed since it was applied);
//  - central: the cached revision is re-planned (published/superseded,
//    materialized, untampered) and must still have the recorded config
//    sha256; then applyCentralMethod;
//  - no pointer: a legacy profile selection in `profileBase` (if given) is
//    restored as before; otherwise nothing is restored.
// Never runs during an experiment (the coordinator's idle transaction).
std::string restoreStartupConfiguration(AppBackend& backend, const std::string& profileBase);

} // namespace backend::app
