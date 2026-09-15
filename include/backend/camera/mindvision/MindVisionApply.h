// Single point that pushes a parsed MindVision Config into the SDK, shared by
// MindVisionCamera::applyJsonConfig (at open, before CameraPlay) and
// CameraControlService::applyMindVisionJsonToCamera (on a temporarily opened
// handle) so the two paths cannot drift apart again.
#pragma once

#include "backend/camera/mindvision/MindVisionConfig.h"

#include <string>

namespace backend::camera::mindvision {

// Applies every field of `cfg` to an OPEN camera handle (CameraHandle is int
// in the MVSDK). Must run before CameraPlay on the streaming path. Individual
// setter failures warn and continue (historical behavior); `firstError`, when
// non-null, receives a message for a CameraSetImageResolution failure only —
// preserving CameraControlService's original error contract. Returns false
// only when the SDK is unavailable at build time (stub).
bool applyConfigToHandle(int hCamera, const Config& cfg, std::string* firstError = nullptr);

} // namespace backend::camera::mindvision
