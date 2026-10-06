// Apply a whole config.json document in the backend (#398 M2c). Qt-free.
//
// The Qt shell applies config.json through its AppConfigWatcher; the React /
// Tauri shell has no config.json applier, so a central method could not be
// applied there exactly. This is that applier, with the watcher's section
// semantics (root processing_contract_version; image_processing with
// difference_threshold winning over bg_subtract_threshold; buffer_threshold;
// experiment_buffer_max_mb; realtime_processing; camera.frame_delivery_mode,
// missing = EveryFrame; pixel_to_micron_factor; autofocus keys; roi).
//
// Fail closed: every section is parsed and validated first; only then are the
// services updated, and finally the exact text is recorded as the applied
// config.json (AppBackend::setLastConfigJson), which is what the method gate
// and run provenance match. A malformed document changes nothing.
// Refused (nothing changed) while a raw recording, live capture, realtime
// processing or autofocus runs — the local-profile apply's precondition: the
// capture worker reads its Config unsynchronised, and raw recording runs with
// the experiment idle.
// Sections this shell cannot apply (dot_grid, which needs the Qt registry
// loader; display_fps, a Qt display setting) are reported, never silently
// dropped.
#pragma once

#include <string>
#include <vector>

namespace backend {
class AppBackend;
}

namespace backend::app {

struct ConfigApplyReport {
    bool ok{false};
    std::string error;                    // why nothing was applied
    std::vector<std::string> applied;     // section names applied
    std::vector<std::string> notApplied;  // present but not applicable in this shell
};

ConfigApplyReport applyConfigDocument(AppBackend& backend, const std::string& configJson);

// Apply a central revision exactly (#398 M2c): refused while an experiment is
// starting, active or stopping (a running experiment keeps its frozen
// method) and under the same conditions as applyConfigDocument; otherwise
// planMethodApply (cached, published/superseded,
// materialized, untampered) then applyConfigDocument.
ConfigApplyReport applyCentralMethod(AppBackend& backend, const std::string& revisionId);

} // namespace backend::app
