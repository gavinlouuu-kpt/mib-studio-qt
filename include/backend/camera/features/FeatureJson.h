#pragma once

#include "backend/camera/features/FeatureTree.h"

#include <cstddef>
#include <string>

// E02 (E-2): the JSON documents of the bridge commands camera_feature_tree,
// camera_feature_read and camera_feature_write (bridge-contract.json
// camera_features, ABI 49). Parsing and rendering only; every device fact comes
// from FeatureTree's guarded calls.
namespace camera::features {

constexpr std::size_t kFeatureRequestMaxBytes = 16384;
constexpr int kFeatureSchemaVersion = 1;

std::string featureTreeJson(GuardedPort& guarded, const std::string& requestJson);
std::string featureReadJson(GuardedPort& guarded, const std::string& requestJson);
std::string featureWriteJson(GuardedPort& guarded, const std::string& requestJson);
// A request refused before any port access (malformed request, backend not
// initialized). Same shape as a read/write reply with outcome "refused".
std::string featureRefusedJson(const std::string& reason);

} // namespace camera::features
