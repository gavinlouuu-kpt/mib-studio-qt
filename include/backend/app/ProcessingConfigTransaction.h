#pragma once
#include <string>
#include <nlohmann/json_fwd.hpp>
#include "backend/processing/ProcessingTypes.h"
namespace backend {
class AppBackend;
}
namespace backend::app {
struct ConfigDocumentSnapshot {
    bool ok{false};
    std::string path, revision, documentJson, error;
};
struct ProcessingConfigTransactionResult {
    bool saved{false}, applied{false}, verified{false}, conflict{false};
    std::string revision, error;
};
services::ProcessingConfig validatedProcessingConfig(const std::string& document, services::ProcessingConfig current);
// The criteria the operator may retune while an experiment is Active (#866 MON-080, Qt tune-panel
// parity): the cell acceptance ranges and switches (area, deformability, ring ratio, area ratio,
// border exclusion, single inner contour) and the target group gate including its Young's modulus
// range. Everything else in image_processing, multi_image included (it fixes the run's saved image
// layout and the realtime mode chosen at Start), stays locked for the whole run.
// True when `imageProcessingPatch` (the value of the "image_processing" key) is an object that
// names only those keys; otherwise `why` (when given) names the first other key as a dotted path.
bool processingPatchIsTuneOnly(const nlohmann::json& imageProcessingPatch, std::string* why = nullptr);
// `base` with exactly those criteria taken from `source`.
services::ProcessingConfig withTuneCriteriaFrom(services::ProcessingConfig base, const services::ProcessingConfig& source);
// Required baseline is SHA256 of raw document bytes. No force overwrite.
// Only image_processing patches; unrelated/unknown document keys survive.
// Like ConfigDocumentStore this is not a cross-process compare-and-swap.
ConfigDocumentSnapshot readConfigDocument(const std::string& path);
// Runs while the experiment is Idle; while it is Active (host processing only) it also runs when the
// patch is tune-only (processingPatchIsTuneOnly): the document gets exactly the patch, and the
// running configuration gets exactly the patch on top of what runs now, never the document's other
// values. Starting, Stopping and Failed refuse everything; so does any other patch while Active.
ProcessingConfigTransactionResult
applyProcessingConfigTransaction(AppBackend& backend, const std::string& path,
                                 const std::string& baselineRevision, const std::string& patchJson);
} // namespace backend::app
