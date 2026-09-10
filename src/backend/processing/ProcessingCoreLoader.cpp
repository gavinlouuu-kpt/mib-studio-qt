#include "backend/processing/ProcessingCoreLoader.h"

#include "backend/processing/ProcessingCoreAbi.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#else
#include <dlfcn.h>
#endif

namespace backend::processing {
namespace {

class Module {
public:
#if defined(_WIN32)
    using Handle = HMODULE;
#else
    using Handle = void*;
#endif

    explicit Module(Handle handle) : handle_(handle) {}
    // Intentionally never unload a processing-core module. Function tables
    // and contexts can be referenced by worker teardown paths; retaining the
    // handle until process exit removes the unsafe unload race.
    ~Module() = default;

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    void* symbol(const char* name) const {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
        return dlsym(handle_, name);
#endif
    }

private:
    Handle handle_{};
};

std::shared_ptr<Module> openModule(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
    const HMODULE handle = LoadLibraryExW(
        path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle) {
        error = "LoadLibraryExW failed with error " + std::to_string(GetLastError());
        return {};
    }
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* detail = dlerror();
        error = detail ? detail : "dlopen failed";
        return {};
    }
#endif
    return std::make_shared<Module>(handle);
}

mib_processing_image_view imageView(const cv::Mat& image) {
    mib_processing_image_view view{};
    view.struct_size = sizeof(view);
    view.width = static_cast<uint32_t>(image.cols);
    view.height = static_cast<uint32_t>(image.rows);
    view.stride_bytes = image.step[0];
    view.data = image.data;
    view.data_size_bytes = image.empty()
                               ? 0
                               : (static_cast<uint64_t>(image.rows - 1) * image.step[0] +
                                  static_cast<uint64_t>(image.cols));
    return view;
}

mib_processing_kernel_config abiConfig(const KernelConfig& config) {
    mib_processing_kernel_config result{};
    result.struct_size = sizeof(result);
    result.gaussian_blur_size = config.gaussianBlurSize;
    result.background_subtract_threshold = config.backgroundSubtractThreshold;
    result.morphology_kernel_size = config.morphologyKernelSize;
    result.morphology_iterations = config.morphologyIterations;
    result.empty_frame_pixel_threshold = config.emptyFramePixelThreshold;
    result.flags = config.backgroundDifferenceMode == BackgroundDifferenceMode::AbsoluteDifference
                       ? MIB_PROCESSING_KERNEL_FLAG_ABSOLUTE_BACKGROUND_DIFFERENCE
                       : 0u;
    return result;
}

mib_processing_roi abiRoi(const KernelRoi& roi) {
    mib_processing_roi result{};
    result.struct_size = sizeof(result);
    result.x = roi.x;
    result.y = roi.y;
    result.width = roi.width;
    result.height = roi.height;
    return result;
}

class DynamicProcessingKernel final : public IProcessingKernel {
    class ContextLease;

public:
    DynamicProcessingKernel(std::shared_ptr<Module> module,
                            mib_processing_api api,
                            mib_processing_context context,
                            ProcessingCoreIdentity identity)
        : module_(std::move(module)), api_(api), identity_(std::move(identity)) {
        contexts_.push_back(context);
    }

    ~DynamicProcessingKernel() override {
        std::scoped_lock lock(contextsMutex_);
        for (auto context : contexts_) {
            if (context && api_.destroy_context) api_.destroy_context(context);
        }
    }

    const ProcessingCoreIdentity& identity() const noexcept override { return identity_; }

    bool processMask(const cv::Mat& gray,
                     const cv::Mat& background,
                     const KernelConfig& config,
                     const KernelRoi& roi,
                     cv::Mat& outputMask,
                     std::string* error) override {
        if (gray.empty() || gray.type() != CV_8UC1) {
            if (error) *error = "dynamic core requires a non-empty CV_8UC1 image";
            return false;
        }
        outputMask = cv::Mat::zeros(gray.rows, gray.cols, CV_8UC1);
        const auto input = imageView(gray);
        const bool useBackground = !background.empty() && background.type() == CV_8UC1 &&
                                   background.size() == gray.size();
        const auto backgroundView = useBackground ? imageView(background)
                                                  : mib_processing_image_view{};
        const auto configValue = abiConfig(config);
        const auto roiValue = abiRoi(roi);
        mib_processing_mutable_image_view output{};
        output.struct_size = sizeof(output);
        output.width = static_cast<uint32_t>(outputMask.cols);
        output.height = static_cast<uint32_t>(outputMask.rows);
        output.stride_bytes = outputMask.step[0];
        output.data = outputMask.data;
        output.data_size_bytes = static_cast<uint64_t>(outputMask.rows - 1) * outputMask.step[0] +
                                 static_cast<uint64_t>(outputMask.cols);
        std::array<char, 512> detail{};
        ContextLease context(*this, detail);
        if (!context) {
            if (error) *error = detail.data();
            outputMask.release();
            return false;
        }
        const auto status = api_.process_mask(
            context.get(), &input, useBackground ? &backgroundView : nullptr, &configValue,
            &roiValue, &output, detail.data(), detail.size());
        detail.back() = '\0';
        if (status != MIB_PROCESSING_STATUS_OK) {
            if (error) *error = detail.data();
            outputMask.release();
            return false;
        }
        return true;
    }

    bool isEmpty(const cv::Mat& gray,
                 const cv::Mat& background,
                 const KernelConfig& config,
                 const KernelRoi& roi,
                 bool& outputIsEmpty,
                 std::string* error) override {
        if (gray.empty() || gray.type() != CV_8UC1) {
            outputIsEmpty = true;
            if (error) *error = "dynamic core requires a non-empty CV_8UC1 image";
            return false;
        }
        const auto input = imageView(gray);
        const bool useBackground = !background.empty() && background.type() == CV_8UC1 &&
                                   background.size() == gray.size();
        const auto backgroundView = useBackground ? imageView(background)
                                                  : mib_processing_image_view{};
        const auto configValue = abiConfig(config);
        const auto roiValue = abiRoi(roi);
        uint8_t empty = 0;
        std::array<char, 512> detail{};
        ContextLease context(*this, detail);
        if (!context) {
            outputIsEmpty = true;
            if (error) *error = detail.data();
            return false;
        }
        const auto status = api_.is_empty(
            context.get(), &input, useBackground ? &backgroundView : nullptr, &configValue,
            &roiValue, &empty, detail.data(), detail.size());
        detail.back() = '\0';
        if (status != MIB_PROCESSING_STATUS_OK) {
            outputIsEmpty = true;
            if (error) *error = detail.data();
            return false;
        }
        outputIsEmpty = empty != 0;
        return true;
    }

    bool reset(std::string* error) override {
        std::array<char, 512> detail{};
        std::scoped_lock lock(contextsMutex_);
        for (auto context : contexts_) {
            const auto status = api_.reset_context(context, detail.data(), detail.size());
            detail.back() = '\0';
            if (status != MIB_PROCESSING_STATUS_OK) {
                if (error) *error = detail.data();
                return false;
            }
        }
        return true;
    }

private:
    class ContextLease {
    public:
        ContextLease(DynamicProcessingKernel& owner, std::array<char, 512>& error)
            : owner_(owner), context_(owner_.acquireContext(error)) {}
        ~ContextLease() {
            if (context_) owner_.releaseContext(context_);
        }
        explicit operator bool() const { return context_ != nullptr; }
        mib_processing_context get() const { return context_; }

    private:
        DynamicProcessingKernel& owner_;
        mib_processing_context context_{nullptr};
    };

    mib_processing_context acquireContext(std::array<char, 512>& error) {
        std::scoped_lock lock(contextsMutex_);
        if (!contexts_.empty()) {
            auto context = contexts_.back();
            contexts_.pop_back();
            return context;
        }
        mib_processing_context context = nullptr;
        const auto status = api_.create_context(&context, error.data(), error.size());
        error.back() = '\0';
        if (status != MIB_PROCESSING_STATUS_OK) {
            return nullptr;
        }
        return context;
    }

    void releaseContext(mib_processing_context context) {
        std::scoped_lock lock(contextsMutex_);
        contexts_.push_back(context);
    }

    // Declared first so the module outlives API calls made by the destructor.
    std::shared_ptr<Module> module_;
    mib_processing_api api_{};
    ProcessingCoreIdentity identity_;
    std::mutex contextsMutex_;
    std::vector<mib_processing_context> contexts_;
};

bool equalsExpected(const std::string& expected, const char* actual) {
    return expected.empty() || (actual && expected == actual);
}

bool sha256Hex(const std::string& value) {
    return value.size() == 64 &&
           value.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

} // namespace

bool verifyProcessingCoreAuthenticode(const std::filesystem::path& path,
                                      const std::string& approvedSubjectPublicKeyInfoSha256,
                                      std::string& error) {
#if !defined(_WIN32)
    (void)path;
    (void)approvedSubjectPublicKeyInfoSha256;
    error = "Authenticode verification is only available on Windows";
    return false;
#else
    if (approvedSubjectPublicKeyInfoSha256.size() != 64 ||
        approvedSubjectPublicKeyInfoSha256.find_first_not_of("0123456789abcdefABCDEF") !=
            std::string::npos) {
        error = "approved signer SPKI SHA-256 is not compiled into the application";
        return false;
    }

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();
    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trustStatus = WinVerifyTrust(nullptr, &policy, &trustData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trustData);
    if (trustStatus != ERROR_SUCCESS) {
        error = "WinVerifyTrust rejected the processing core (status " +
                std::to_string(static_cast<unsigned long>(trustStatus)) + ")";
        return false;
    }

    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0, contentType = 0, formatType = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType,
                          &formatType, &store, &message, nullptr)) {
        error = "cannot inspect Authenticode signer";
        return false;
    }

    DWORD signerSize = 0;
    CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize);
    std::vector<uint8_t> signerBuffer(signerSize);
    bool ok = signerSize > 0 &&
              CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(),
                               &signerSize);
    PCCERT_CONTEXT certificate = nullptr;
    if (ok) {
        const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(signerBuffer.data());
        CERT_INFO match{};
        match.Issuer = signer->Issuer;
        match.SerialNumber = signer->SerialNumber;
        certificate = CertFindCertificateInStore(store, encoding, 0, CERT_FIND_SUBJECT_CERT,
                                                 &match, nullptr);
        ok = certificate != nullptr;
    }
    BYTE* encodedSubjectPublicKeyInfo = nullptr;
    DWORD encodedSubjectPublicKeyInfoSize = 0;
    if (ok) {
        ok = CryptEncodeObjectEx(
            X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
            &certificate->pCertInfo->SubjectPublicKeyInfo, CRYPT_ENCODE_ALLOC_FLAG,
            nullptr, &encodedSubjectPublicKeyInfo, &encodedSubjectPublicKeyInfoSize);
    }
    std::array<uint8_t, 32> subjectPublicKeyInfoHash{};
    DWORD subjectPublicKeyInfoHashSize = static_cast<DWORD>(subjectPublicKeyInfoHash.size());
    if (ok) {
        ok = CryptHashCertificate(
                 0, CALG_SHA_256, 0, encodedSubjectPublicKeyInfo,
                 encodedSubjectPublicKeyInfoSize, subjectPublicKeyInfoHash.data(),
                 &subjectPublicKeyInfoHashSize) &&
             subjectPublicKeyInfoHashSize == subjectPublicKeyInfoHash.size();
    }
    if (encodedSubjectPublicKeyInfo) LocalFree(encodedSubjectPublicKeyInfo);
    if (certificate) CertFreeCertificateContext(certificate);
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
    if (!ok) {
        error = "cannot resolve Authenticode signer public key";
        return false;
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (uint8_t byte : subjectPublicKeyInfoHash) {
        actual.push_back(hex[byte >> 4u]);
        actual.push_back(hex[byte & 0x0fu]);
    }
    std::string expected = approvedSubjectPublicKeyInfoSha256;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (actual != expected) {
        error = "Authenticode signer public key is not approved";
        return false;
    }
    return true;
#endif
}

ProcessingCoreLoadResult loadProcessingCorePlugin(
    const std::filesystem::path& absolutePluginPath,
    const ProcessingCoreLoadRequirements& requirements) {
    ProcessingCoreLoadResult result;
    using Failure = ProcessingCoreLoadFailure;
    const auto hostIdentity = bundledProcessingCoreIdentity();
    if (requirements.expectedEngineAbiVersion != MIB_PROCESSING_ENGINE_ABI_VERSION) {
        result.failure = Failure::UnsupportedAbi;
        result.error = "processing core metadata requires ABI " +
            std::to_string(requirements.expectedEngineAbiVersion) + "; host supports " +
            std::to_string(MIB_PROCESSING_ENGINE_ABI_VERSION);
        return result;
    }
    if (requirements.expectedContractVersion != MIB_PROCESSING_CONTRACT_VERSION) {
        result.failure = Failure::UnsupportedContract;
        result.error = "processing core metadata requires contract " +
            std::to_string(requirements.expectedContractVersion) + "; host supports " +
            std::to_string(MIB_PROCESSING_CONTRACT_VERSION);
        return result;
    }
    if (requirements.expectedRuntimeFingerprint.empty() ||
        requirements.expectedRuntimeFingerprint != hostIdentity.runtimeFingerprint) {
        result.failure = Failure::RuntimeConstraint;
        result.error = "processing core metadata is incompatible with the host runtime";
        return result;
    }
    if (requirements.expectedVersion.empty()) {
        result.failure = Failure::InvalidMetadata;
        result.error = "processing core expected version is required";
        return result;
    }
    if (!sha256Hex(requirements.artifactSha256) ||
        !sha256Hex(requirements.manifestSha256)) {
        result.failure = Failure::InvalidMetadata;
        result.error = "processing core artifact and manifest SHA-256 are required";
        return result;
    }
    std::error_code ec;
    if (!absolutePluginPath.is_absolute()) {
        result.failure = Failure::ArtifactFailure;
        result.error = "processing core path must be absolute";
        return result;
    }
    if (!std::filesystem::is_regular_file(absolutePluginPath, ec) || ec) {
        result.failure = Failure::ArtifactFailure;
        result.error = "processing core path is not a readable regular file";
        return result;
    }
    std::string hashError;
    const std::string actual = processingCoreFileSha256(absolutePluginPath, &hashError);
    std::string expected = requirements.artifactSha256;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (actual.empty() || actual != expected) {
        result.failure = Failure::ArtifactFailure;
        result.error = actual.empty() ? "processing core SHA-256 failed: " + hashError
                                      : "processing core SHA-256 mismatch";
        return result;
    }
    if (!requirements.trustVerifier) {
        result.failure = Failure::SignatureFailure;
        result.error = "processing core trust verifier is required";
        return result;
    }
    std::string trustError;
    if (!requirements.trustVerifier(absolutePluginPath, trustError)) {
        result.failure = Failure::SignatureFailure;
        result.error = "processing core trust verification failed: " + trustError;
        return result;
    }

    auto module = openModule(absolutePluginPath, result.error);
    if (!module) {
        result.failure = Failure::ModuleLoadFailure;
        return result;
    }
    const auto getApi = reinterpret_cast<mib_processing_get_api_fn>(
        module->symbol(MIB_PROCESSING_GET_API_SYMBOL));
    if (!getApi) {
        result.failure = Failure::UnsupportedAbi;
        result.error = "processing core does not export " MIB_PROCESSING_GET_API_SYMBOL;
        return result;
    }

    mib_processing_api api{};
    std::array<char, 512> detail{};
    const auto status = getApi(requirements.expectedEngineAbiVersion, sizeof(api), &api,
                               detail.data(), detail.size());
    detail.back() = '\0';
    if (status != MIB_PROCESSING_STATUS_OK) {
        result.failure = Failure::UnsupportedAbi;
        result.error = std::string("processing core ABI negotiation failed: ") + detail.data();
        return result;
    }
    if (api.struct_size < sizeof(mib_processing_api) ||
        api.engine_abi_version != requirements.expectedEngineAbiVersion || !api.descriptor ||
        !api.create_context || !api.destroy_context || !api.reset_context || !api.process_mask ||
        !api.is_empty || !api.self_test) {
        result.failure = Failure::InvalidApi;
        result.error = "processing core returned an incomplete API table";
        return result;
    }
    const auto* descriptor = api.descriptor();
    if (!descriptor || descriptor->struct_size < sizeof(mib_processing_core_descriptor) ||
        descriptor->engine_abi_version != requirements.expectedEngineAbiVersion ||
        descriptor->contract_version != requirements.expectedContractVersion ||
        !equalsExpected(requirements.expectedVersion, descriptor->core_version) ||
        !equalsExpected(requirements.expectedRuntimeFingerprint, descriptor->runtime_fingerprint)) {
        result.failure = Failure::IdentityMismatch;
        result.error = "processing core descriptor does not satisfy the requested identity";
        return result;
    }
    detail.fill(0);
    if (api.self_test(detail.data(), detail.size()) != MIB_PROCESSING_STATUS_OK) {
        detail.back() = '\0';
        result.failure = Failure::SelfTestFailure;
        result.error = std::string("processing core self-test failed: ") + detail.data();
        return result;
    }

    mib_processing_context context = nullptr;
    detail.fill(0);
    if (api.create_context(&context, detail.data(), detail.size()) != MIB_PROCESSING_STATUS_OK ||
        !context) {
        detail.back() = '\0';
        result.failure = Failure::ContextCreationFailure;
        result.error = std::string("processing core context creation failed: ") + detail.data();
        return result;
    }

    ProcessingCoreIdentity identity;
    identity.version = descriptor->core_version ? descriptor->core_version : "";
    identity.contractVersion = descriptor->contract_version;
    identity.engineAbiVersion = descriptor->engine_abi_version;
    identity.artifactSha256 = requirements.artifactSha256;
    identity.releaseTag = requirements.releaseTag;
    identity.manifestSha256 = requirements.manifestSha256;
    identity.source = "plugin";
    identity.buildId = descriptor->build_id ? descriptor->build_id : "";
    identity.runtimeFingerprint =
        descriptor->runtime_fingerprint ? descriptor->runtime_fingerprint : "";
    result.kernel = std::make_shared<DynamicProcessingKernel>(
        std::move(module), api, context, std::move(identity));
    return result;
}

} // namespace backend::processing
