#include "backend/app/ProcessingCoreTrust.h"
#include <cstdlib>
#include <algorithm>
#include <cctype>
namespace backend::app {
namespace {
#ifndef NDEBUG
std::string overridePin(const char* name, const std::string& fallback) {
    const char* raw = std::getenv(name);
    if (!raw) return fallback;
    std::string text(raw);
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(),
                                              [](unsigned char c) { return std::isspace(c); }));
    text.erase(std::find_if_not(text.rbegin(), text.rend(),
                                [](unsigned char c) { return std::isspace(c); })
                   .base(),
               text.end());
    return text.empty() ? fallback : text;
}
#endif
} // namespace

std::function<bool(const std::filesystem::path&, std::string&)>
processingCoreTrustVerifier(const ProcessingCoreSignaturePolicy& p) {
#ifndef NDEBUG
    const auto* allow = std::getenv("MIB_STUDIO_ALLOW_UNSIGNED_PROCESSING_CORE");
    if (allow && std::string(allow) == "1") return [](const auto&, std::string&) { return true; };
#endif
    if (!p.required)
        return [](const auto&, std::string& e) {
            e = "Manifest does not require an artifact signature";
            return false;
        };
#if defined(_WIN32)
    if (p.scheme != "authenticode")
        return [](const auto&, std::string& e) {
            e = "Unsupported Windows core signature scheme";
            return false;
        };
    std::string approved = MIB_PROCESSING_CORE_SIGNER_SPKI_SHA256;
#ifndef NDEBUG
    approved = overridePin("MIB_STUDIO_PROCESSING_CORE_SIGNER_SPKI_SHA256", approved);
#endif
    return [approved](const auto& path, std::string& e) {
        return processing::verifyProcessingCoreAuthenticode(path, approved, e);
    };
#elif defined(__linux__)
    if (p.scheme != "ed25519")
        return [](const auto&, std::string& e) {
            e = "Unsupported Linux core signature scheme";
            return false;
        };
    std::string approved = MIB_PROCESSING_CORE_ED25519_SPKI_SHA256;
#ifndef NDEBUG
    approved = overridePin("MIB_STUDIO_PROCESSING_CORE_ED25519_SPKI_SHA256", approved);
#endif
    processing::ProcessingCoreDetachedSignature signature{p.publicKeySpkiBase64, p.signatureBase64};
    return [approved, signature](const auto& path, std::string& e) {
        return processing::verifyProcessingCoreEd25519(path, signature, approved, e);
    };
#else
    return [](const auto&, std::string& e) {
        e = "Processing core signature verification unsupported on this platform";
        return false;
    };
#endif
}
} // namespace backend::app
