#pragma once
#include <string>
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
// Required baseline is SHA256 of raw document bytes. No force overwrite.
// Only image_processing patches; unrelated/unknown document keys survive.
// Like ConfigDocumentStore this is not a cross-process compare-and-swap.
ConfigDocumentSnapshot readConfigDocument(const std::string& path);
ProcessingConfigTransactionResult
applyProcessingConfigTransaction(AppBackend& backend, const std::string& path,
                                 const std::string& baselineRevision, const std::string& patchJson);
} // namespace backend::app
