#pragma once
#include "backend/processing/ProcessingCoreLoader.h"
namespace backend::app {
struct ProcessingCoreSignaturePolicy {
    bool required{false};
    std::string scheme, publicKeySpkiBase64, signatureBase64;
};
// Shared Qt/Tauri production policy: trust is pinned at compile time, never
// supplied by catalog JSON. Existing debug-only overrides remain debug-only.
std::function<bool(const std::filesystem::path&, std::string&)>
processingCoreTrustVerifier(const ProcessingCoreSignaturePolicy& policy);
} // namespace backend::app
