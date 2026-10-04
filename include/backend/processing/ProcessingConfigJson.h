#pragma once

#include "backend/processing/ProcessingTypes.h"

// Distro packages of nlohmann/json (e.g. EPEL json-devel in the manylinux
// wheel container) ship only the single-include json.hpp.
#if __has_include(<nlohmann/json_fwd.hpp>)
#include <nlohmann/json_fwd.hpp>
#else
#include <nlohmann/json.hpp>
#endif

#include <string>

namespace backend::processing::config_json
{

    // Qt-free (de)serialization of services::ProcessingConfig using the exact
    // `image_processing` schema of config.json (BE-3, issue #273) — the same
    // key layout the Qt ConfigTabs read/write, so existing files stay
    // compatible.
    //
    // Merge semantics: fromJson only overwrites fields that are PRESENT in the
    // JSON — absent fields keep their current values, so a partial document
    // can never silently substitute defaults. Unknown keys are ignored
    // (additive contract). Returns false with `errorOut` on malformed values.

    nlohmann::json toJson(const services::ProcessingConfig &config);

    bool fromJson(const nlohmann::json &json,
                  services::ProcessingConfig &config,
                  std::string *errorOut = nullptr);

    // The full science config handed to an engine-ABI-v2 core
    // (mib_processing_kernel_config_v2::science_config_json): toJson() plus an
    // "abi_v2" object with the fields the persisted schema does not carry
    // (processing_contract_version, the Laplacian gate, the runtime channel
    // band). Host and plugin both use these two functions, so the encoding
    // cannot drift.
    nlohmann::json toScienceJson(const services::ProcessingConfig &config);

    bool fromScienceJson(const nlohmann::json &json,
                         services::ProcessingConfig &config,
                         std::string *errorOut = nullptr);

} // namespace backend::processing::config_json
