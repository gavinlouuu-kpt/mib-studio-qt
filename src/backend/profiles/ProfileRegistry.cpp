#include "backend/profiles/ProfileRegistry.h"
#include "backend/processing/ProcessingCoreSha256.h"

#include <cmath>
#include <nlohmann/json.hpp>
#include <set>

namespace backend::profiles {
namespace {
using Json = nlohmann::json;
constexpr size_t kMaxContentBytes = 1024 * 1024;

Json parse(const std::string& text) {
    if (text.size() > kMaxContentBytes)
        throw RegistryError(RegistryErrorCode::Invalid, "Method exceeds 1 MiB document limit");
    std::vector<std::set<std::string>> keys;
    try {
        return Json::parse(text, [&keys](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 64)
                throw RegistryError(RegistryErrorCode::Invalid, "Method nesting exceeds 64");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key &&
                !keys.back().insert(value.get<std::string>()).second)
                throw RegistryError(RegistryErrorCode::Invalid, "Duplicate method JSON key");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
    } catch (const Json::exception&) {
        throw RegistryError(RegistryErrorCode::Invalid, "Invalid method JSON");
    }
}

void normalize(Json& value) {
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number))
            throw RegistryError(RegistryErrorCode::Invalid, "Non-finite number");
        if (std::abs(number) <= 9007199254740991.0 && std::trunc(number) == number)
            value = static_cast<int64_t>(number);
    } else if (value.is_structured()) {
        for (auto& item : value)
            normalize(item);
    }
}

void requireObject(const Json& value) {
    if (!value.is_object())
        throw RegistryError(RegistryErrorCode::Invalid, "Expected method JSON object");
}
} // namespace

const char* toString(CentralState state) {
    switch (state) {
    case CentralState::Submitted:
        return "submitted";
    case CentralState::Approved:
        return "approved";
    case CentralState::Rejected:
        return "rejected";
    case CentralState::Published:
        return "published";
    case CentralState::Superseded:
        return "superseded";
    case CentralState::Archived:
        return "archived";
    case CentralState::Revoked:
        return "revoked";
    }
    throw RegistryError(RegistryErrorCode::Invalid, "Unknown central state");
}

CentralState centralStateFromString(const std::string& state) {
    for (auto candidate : {CentralState::Submitted, CentralState::Approved, CentralState::Rejected,
                           CentralState::Published, CentralState::Superseded,
                           CentralState::Archived, CentralState::Revoked})
        if (state == toString(candidate)) return candidate;
    throw RegistryError(RegistryErrorCode::Invalid, "Unknown central state");
}

std::string contentHash(const std::string& bytes) {
    return processing::processingCoreBytesSha256(reinterpret_cast<const uint8_t*>(bytes.data()),
                                                 bytes.size());
}

std::string canonicalMethod(const std::string& configJson, const std::string& cameraScript,
                            const std::string& processingCoreId, int processingContractVersion,
                            const std::string& hardwareCompatibilityJson) {
    auto config = parse(configJson);
    auto hardware = parse(hardwareCompatibilityJson);
    requireObject(config);
    requireObject(hardware);
    if (!config.contains("config_schema_version") ||
        !config["config_schema_version"].is_number_integer() ||
        config["config_schema_version"] != 1 || processingCoreId.empty() ||
        processingContractVersion <= 0)
        throw RegistryError(RegistryErrorCode::Invalid,
                            "Unsupported config schema or missing processing identity");
    Json envelope = {{"method_schema_version", 1},
                     {"config", config},
                     {"camera_script", cameraScript},
                     {"processing_core_id", processingCoreId},
                     {"processing_contract_version", processingContractVersion},
                     {"declared_hardware_compatibility", hardware}};
    normalize(envelope);
    std::string result;
    try {
        result = envelope.dump();
    } catch (const Json::exception&) {
        throw RegistryError(RegistryErrorCode::Invalid, "Invalid UTF-8 method");
    }
    if (result.size() > kMaxContentBytes)
        throw RegistryError(RegistryErrorCode::Invalid, "Method exceeds 1 MiB document limit");
    return result;
}

void verifyRevision(const Revision& revision) {
    if (revision.methodId.empty() || revision.revisionId.empty() || revision.projectId.empty() ||
        revision.revisionNumber == 0 || revision.metadataVersion == 0)
        throw RegistryError(RegistryErrorCode::Invalid, "Missing immutable revision identity");
    if (contentHash(revision.canonicalContent) != revision.contentHash)
        throw RegistryError(RegistryErrorCode::Integrity, "Method content hash mismatch");
    auto envelope = parse(revision.canonicalContent);
    try {
        if (envelope.size() != 6 || envelope.at("method_schema_version") != 1 ||
            !envelope.at("processing_contract_version").is_number_integer() ||
            envelope.at("processing_contract_version").get<int64_t>() > INT32_MAX ||
            envelope.at("processing_contract_version").get<int64_t>() <= 0 ||
            canonicalMethod(
                envelope.at("config").dump(), envelope.at("camera_script").get<std::string>(),
                envelope.at("processing_core_id").get<std::string>(),
                envelope.at("processing_contract_version").get<int>(),
                envelope.at("declared_hardware_compatibility").dump()) != revision.canonicalContent)
            throw RegistryError(RegistryErrorCode::Invalid,
                                "Noncanonical or unsupported method envelope");
    } catch (const Json::exception&) {
        throw RegistryError(RegistryErrorCode::Invalid, "Invalid method envelope");
    }
}
} // namespace backend::profiles
