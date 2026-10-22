// Compile every module operation against the actual vendor SDK headers. No main,
// object construction, SDK execution, second device handle or hardware access.
#include "backend/camera/features/EGrabberFeaturePort.h"
template class backend::camera::features::EGrabberFeaturePort<
    Euresys::EGrabber<Euresys::CallbackOnDemand>>;
