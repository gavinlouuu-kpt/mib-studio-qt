#pragma once

// Process-wide owner of the Euresys GenTL producer handle (EGenTL).
//
// Two facts drive this:
//  1. Only one EGenTL may exist per process at a time. A second construction
//     while another is alive fails with GC_ERR_RESOURCE_IN_USE
//     ("GenTL error -1004, GCInitLib: Requested resource is already in use").
//  2. The MindVision SDK ships a CoaXPress plugin (CXPCamera_X64.Interface)
//     that loads coaxlink.cti and calls GCInitLib itself during
//     CameraEnumerateDevice(). If it gets there first, every later EGrabber
//     open in the process fails with -1004 for as long as the process lives.
//     If our handle is already open, the plugin's attempt fails harmlessly
//     and EGrabber discovery and acquisition keep working.
//
// So every EGrabber user shares this one handle, and MindVision enumeration
// calls ensureSharedGenTL() before touching its SDK. The handle is kept open
// until process exit on purpose; holding the TL open does not hold any
// interface, device, or stream, so other processes are unaffected.
//
// Only compiled when MIB_HAS_EGRABBER (Windows builds with the eGrabber SDK).

#include <memory>

namespace Euresys {
class EGenTL;
}

namespace backend::camera::egrabber {

// Returns the shared producer handle, opening it on first use. Throws the
// Euresys exception if the producer cannot be opened (no driver/SDK).
std::shared_ptr<Euresys::EGenTL> sharedGenTL();

// Opens the shared handle if it is not open yet. Never throws; returns
// whether a handle is held afterwards.
bool ensureSharedGenTL() noexcept;

} // namespace backend::camera::egrabber
