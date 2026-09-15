#pragma once
#include <functional>

namespace backend::services {
// Owned by one camera generation. Operations run on capture start/stop paths,
// never on the frame hot path. Generator service serializes manual access.
struct IlluminationSession {
    std::function<bool()> prepare; // connect, gate off, configure
    std::function<bool()> enable;  // only after the camera is armed
    std::function<bool()> disable; // gate off, release ownership
};
} // namespace backend::services
