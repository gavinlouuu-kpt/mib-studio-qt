// Single point that pushes a parsed MindVision Config into the SDK, shared by
// MindVisionCamera::applyJsonConfig (at open, before CameraPlay) and
// CameraControlService::applyMindVisionJsonToCamera (on a temporarily opened
// handle) so the two paths cannot drift apart again.
#pragma once

#include "backend/camera/mindvision/MindVisionConfig.h"

#include <string>

namespace backend::camera::mindvision {

// Applies every field to an OPEN handle, before CameraPlay. Illuminated rig
// profiles fail closed if any setter fails; legacy manual profiles retain
// warn-and-continue behavior. firstError receives the first failed setter.
// The SDK-unavailable implementation always returns false.
bool applyConfigToHandle(int hCamera, const Config& cfg, std::string* firstError = nullptr);

} // namespace backend::camera::mindvision
