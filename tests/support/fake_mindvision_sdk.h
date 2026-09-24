// Scripted fake of the MindVision SDK seam (backend/camera/mindvision/
// MindVisionSdk.h) for conversion/geometry/lifecycle fault injection
// (issue #366). No vendor headers, no hardware.
#pragma once

#include "backend/camera/mindvision/MindVisionSdk.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mib::test {

namespace mv = backend::camera::mindvision;

struct FakeMindVisionSdk {
    // ---- script ----------------------------------------------------------
    int deviceCount = 1;
    int width = 512;
    int height = 96;
    mv::SdkStatus setIspOutFormatStatus = mv::kSdkSuccess;
    mv::SdkStatus getIspOutFormatStatus = mv::kSdkSuccess;
    // Effective format the readback reports (defaults to what was set).
    std::uint32_t readbackFormatOverride = 0;
    bool readbackOverrideEnabled = false;
    mv::SdkStatus playStatus = mv::kSdkSuccess;
    bool allocFails = false;
    bool provideNewestApi = false;
    // Frames to deliver, in order. Each has its own header so tests can
    // change geometry mid-session.
    struct ScriptedFrame {
        mv::SdkFrameInfo head;
        std::vector<std::uint8_t> payload;
    };
    std::deque<ScriptedFrame> frames;
    // When true, getImageBuffer blocks until stop() is called or releaseGrab().
    bool blockGrab = false;
    // When true, a blocked getImageBuffer ignores stop() (models a wedged
    // driver) until releaseGrab() is called explicitly.
    bool ignoreStopWhileBlocked = false;

    // ---- observations ----------------------------------------------------
    std::atomic<int> setIspOutFormatCalls{0};
    std::atomic<int> getIspOutFormatCalls{0};
    std::atomic<int> allocCalls{0};
    std::atomic<int> freeCalls{0};
    std::atomic<int> playCalls{0};
    std::atomic<int> stopCalls{0};
    std::atomic<int> unInitCalls{0};
    std::atomic<int> imageProcessCalls{0};
    std::atomic<int> getImageBufferCalls{0};
    std::atomic<int> releaseCalls{0};
    std::atomic<int> outstandingBuffers{0};
    std::atomic<std::size_t> lastAllocBytes{0};
    std::atomic<bool> grabInFlight{false};
    std::atomic<bool> unInitWhileGrabInFlight{false};
    std::atomic<int> handleCounter{100};
    std::uint32_t requestedFormat = 0;

    // Sequencing helpers for the blocked-grab tests.
    std::mutex mutex;
    std::condition_variable cv;
    bool stopped = false;
    bool released = false;
    bool blocked = false;

    void releaseGrab()
    {
        {
            std::lock_guard<std::mutex> lk(mutex);
            released = true;
        }
        cv.notify_all();
    }
    bool waitUntilBlocked(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mutex);
        return cv.wait_for(lk, timeout, [&] { return blocked; });
    }

    static ScriptedFrame frame(int w, int h, std::uint8_t fill, std::uint32_t ts = 1)
    {
        ScriptedFrame f;
        f.head.width = w;
        f.head.height = h;
        f.head.mediaType = mv::kMediaTypeMono8;
        f.head.bytes = static_cast<std::uint32_t>(w * h);
        f.head.timeStamp = ts;
        f.payload.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), fill);
        for (std::size_t i = 0; i < f.payload.size(); ++i) {
            f.payload[i] = static_cast<std::uint8_t>((fill + i) & 0xFF);
        }
        return f;
    }

    std::shared_ptr<const mv::SdkOps> ops()
    {
        auto o = std::make_shared<mv::SdkOps>();
        o->sdkInit = [] { return mv::kSdkSuccess; };
        o->enumerate = [this](int& count) {
            count = deviceCount;
            return count > 0 ? mv::kSdkSuccess : -1;
        };
        o->init = [this](int index, int& handle) {
            if (index < 0 || index >= deviceCount) return -2;
            handle = handleCounter.fetch_add(1);
            return mv::kSdkSuccess;
        };
        o->getCapability = [](int, mv::SdkCapability& cap) {
            cap.monoSensor = true;
            return mv::kSdkSuccess;
        };
        o->applyConfig = [](int, const mv::Config&) { return true; };
        o->getImageResolution = [this](int, int& w, int& h) {
            w = width;
            h = height;
            return mv::kSdkSuccess;
        };
        o->setIspOutFormat = [this](int, std::uint32_t fmt) {
            setIspOutFormatCalls++;
            requestedFormat = fmt;
            return setIspOutFormatStatus;
        };
        o->getIspOutFormat = [this](int, std::uint32_t& fmt) {
            getIspOutFormatCalls++;
            if (getIspOutFormatStatus != mv::kSdkSuccess) return getIspOutFormatStatus;
            fmt = readbackOverrideEnabled ? readbackFormatOverride : requestedFormat;
            return mv::kSdkSuccess;
        };
        o->alignMalloc = [this](std::size_t bytes, int) -> std::uint8_t* {
            allocCalls++;
            lastAllocBytes = bytes;
            if (allocFails) return nullptr;
            return static_cast<std::uint8_t*>(std::malloc(bytes));
        };
        o->alignFree = [this](std::uint8_t* p) {
            freeCalls++;
            std::free(p);
        };
        o->play = [this](int) {
            playCalls++;
            return playStatus;
        };
        o->stop = [this](int) {
            stopCalls++;
            {
                std::lock_guard<std::mutex> lk(mutex);
                stopped = true;
            }
            cv.notify_all();
            return mv::kSdkSuccess;
        };
        o->unInit = [this](int) {
            unInitCalls++;
            if (grabInFlight.load()) unInitWhileGrabInFlight = true;
            return mv::kSdkSuccess;
        };
        auto grab = [this](int, mv::SdkFrameInfo& info, std::uint8_t*& buffer, unsigned timeoutMs) {
            getImageBufferCalls++;
            grabInFlight = true;
            struct Clear { std::atomic<bool>& f; ~Clear() { f = false; } } clear{grabInFlight};
            if (blockGrab) {
                std::unique_lock<std::mutex> lk(mutex);
                blocked = true;
                cv.notify_all();
                cv.wait(lk, [&] { return released || (stopped && !ignoreStopWhileBlocked); });
                blocked = false;
                if (!released) return mv::kSdkTimeout;
                released = false;
            }
            std::lock_guard<std::mutex> lk(mutex);
            if (frames.empty()) {
                return timeoutMs == 0 ? mv::kSdkTimeout : mv::kSdkTimeout;
            }
            auto f = std::move(frames.front());
            frames.pop_front();
            info = f.head;
            auto* raw = static_cast<std::uint8_t*>(std::malloc(f.payload.size() + 1));
            std::memcpy(raw, f.payload.data(), f.payload.size());
            buffer = raw;
            outstandingBuffers++;
            return mv::kSdkSuccess;
        };
        o->getImageBuffer = grab;
        if (provideNewestApi) o->getImageBufferNewest = grab;
        o->releaseImageBuffer = [this](int, std::uint8_t* b) {
            releaseCalls++;
            outstandingBuffers--;
            std::free(b);
            return mv::kSdkSuccess;
        };
        o->imageProcess = [this](int, std::uint8_t* in, std::uint8_t* out, mv::SdkFrameInfo& info) {
            imageProcessCalls++;
            // Model the ISP: copy exactly width*height*bpp(effective) bytes.
            // A real SDK would overrun an undersized `out`; the camera must
            // never let us get here with one (validated by the test harness
            // through lastAllocBytes vs. info).
            const std::size_t n = static_cast<std::size_t>(info.width) *
                                  static_cast<std::size_t>(info.height);
            std::memcpy(out, in, n);
            return mv::kSdkSuccess;
        };
        o->getFrameStatistic = [](int, mv::SdkFrameStatistic& s) {
            s = {};
            return mv::kSdkSuccess;
        };
        o->setOutputIoMode = [](int, int, int) { return mv::kSdkSuccess; };
        o->setIoStateEx = [](int, int, unsigned) { return mv::kSdkSuccess; };
        o->softTrigger = [](int) { return mv::kSdkSuccess; };
        return o;
    }
};

} // namespace mib::test
