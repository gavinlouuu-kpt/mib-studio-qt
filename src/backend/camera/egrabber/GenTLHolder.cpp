#include "backend/camera/egrabber/GenTLHolder.h"

#include <EGrabber.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <mutex>

namespace backend::camera::egrabber {

namespace {

std::mutex& holderMutex()
{
    static std::mutex m;
    return m;
}

// Intentionally never released: see GenTLHolder.h. The shared_ptr copies
// handed out keep the same object; the last one to go is this static at exit.
std::shared_ptr<Euresys::EGenTL>& holderSlot()
{
    static std::shared_ptr<Euresys::EGenTL> slot;
    return slot;
}

} // namespace

std::shared_ptr<Euresys::EGenTL> sharedGenTL()
{
    std::lock_guard<std::mutex> lock(holderMutex());
    auto& slot = holderSlot();
    if (!slot) {
        slot = std::make_shared<Euresys::EGenTL>();
        SPDLOG_INFO("GenTLHolder: opened the process-wide Euresys GenTL producer handle");
    }
    return slot;
}

bool ensureSharedGenTL() noexcept
{
    try {
        (void)sharedGenTL();
        return true;
    } catch (const std::exception& ex) {
        SPDLOG_WARN("GenTLHolder: Euresys GenTL producer not available: {}", ex.what());
        return false;
    } catch (...) {
        SPDLOG_WARN("GenTLHolder: Euresys GenTL producer not available (unknown error)");
        return false;
    }
}

} // namespace backend::camera::egrabber
