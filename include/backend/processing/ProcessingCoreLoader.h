#pragma once

#include "backend/processing/IProcessingKernel.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace backend::processing {

struct ProcessingCoreLoadRequirements {
    std::string expectedVersion;
    uint32_t expectedContractVersion{1};
    uint32_t expectedEngineAbiVersion{1};
    std::string expectedRuntimeFingerprint;
    std::string artifactSha256;
    std::string releaseTag;
    std::string manifestSha256;
    // The desktop supplies the platform's required artifact-signature verifier.
    // Tests can inject a deterministic verifier without weakening production.
    std::function<bool(const std::filesystem::path&, std::string&)> trustVerifier;
};

enum class ProcessingCoreLoadFailure {
    None, InvalidMetadata, UnsupportedAbi, UnsupportedContract, RuntimeConstraint,
    ArtifactFailure, SignatureFailure, ModuleLoadFailure, InvalidApi,
    IdentityMismatch, SelfTestFailure, ContextCreationFailure
};

struct ProcessingCoreLoadResult {
    std::shared_ptr<IProcessingKernel> kernel;
    std::string error;
    ProcessingCoreLoadFailure failure{ProcessingCoreLoadFailure::None};

    explicit operator bool() const noexcept { return kernel != nullptr; }
};

ProcessingCoreLoadResult loadProcessingCorePlugin(
    const std::filesystem::path& absolutePluginPath,
    const ProcessingCoreLoadRequirements& requirements);

// Qt-free streaming SHA-256 used before a native module is loaded.
std::string processingCoreFileSha256(const std::filesystem::path& path,
                                     std::string* error = nullptr);
std::string processingCoreBytesSha256(const uint8_t* bytes, size_t count);

// Windows production verifier: validates the embedded Authenticode chain and
// requires the leaf certificate's DER SubjectPublicKeyInfo SHA-256 to match
// the compiled allowlist value. Non-Windows builds fail closed.
bool verifyProcessingCoreAuthenticode(const std::filesystem::path& path,
                                      const std::string& approvedSubjectPublicKeyInfoSha256,
                                      std::string& error);

// Offline Ed25519 detached signature transported by the immutable registry
// manifest: the base64 DER SubjectPublicKeyInfo of the signer and the base64
// raw 64-byte signature over the exact artifact bytes.
struct ProcessingCoreDetachedSignature {
    std::string publicKeySpkiDerBase64;
    std::string signatureBase64;
};

// Linux production verifier: validates the detached Ed25519 signature over
// the artifact bytes and requires the signer public key's DER
// SubjectPublicKeyInfo SHA-256 to match the compiled allowlist value.
// Non-Linux builds fail closed.
bool verifyProcessingCoreEd25519(const std::filesystem::path& path,
                                 const ProcessingCoreDetachedSignature& signature,
                                 const std::string& approvedSubjectPublicKeyInfoSha256,
                                 std::string& error);

} // namespace backend::processing
