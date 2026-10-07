// USBTMC SCPI transport on Windows through NI-VISA (the driver the SSG3021X
// documentation installs). visa64.dll / visa32.dll is loaded at runtime so the
// build has no VISA SDK dependency and a PC without VISA fails open() with a
// clear message instead of failing to start the app.
#include "backend/services/ScpiTransport.h"

#include <spdlog/spdlog.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace backend::services::scpi {

namespace {

// visatype.h subset: ViSession/ViObject are 32-bit unsigned, ViStatus is
// 32-bit signed, functions are __stdcall. Only the entry points used here.
using ViStatus = int32_t;
using ViSession = uint32_t;
using ViFindList = uint32_t;
using ViUInt32 = uint32_t;
using ViAttr = uint32_t;
using ViAttrState = uintptr_t;
constexpr ViUInt32 kViNull = 0;
constexpr ViAttr kViAttrTmoValue = 0x3FFF001A;
constexpr ViAttr kViAttrTermcharEn = 0x3FFF0038;
constexpr ViAttr kViAttrTermchar = 0x3FFF0018;
constexpr ViStatus kViSuccessMaxCnt = 0x3FFF0006;

using PFN_viOpenDefaultRM = ViStatus(__stdcall*)(ViSession*);
using PFN_viOpen = ViStatus(__stdcall*)(ViSession, const char*, ViUInt32, ViUInt32, ViSession*);
using PFN_viClose = ViStatus(__stdcall*)(ViUInt32);
using PFN_viWrite = ViStatus(__stdcall*)(ViSession, const unsigned char*, ViUInt32, ViUInt32*);
using PFN_viRead = ViStatus(__stdcall*)(ViSession, unsigned char*, ViUInt32, ViUInt32*);
using PFN_viSetAttribute = ViStatus(__stdcall*)(ViUInt32, ViAttr, ViAttrState);
using PFN_viFindRsrc = ViStatus(__stdcall*)(ViSession, const char*, ViFindList*, ViUInt32*, char*);
using PFN_viFindNext = ViStatus(__stdcall*)(ViFindList, char*);

struct VisaApi {
    HMODULE module{nullptr};
    PFN_viOpenDefaultRM openDefaultRM{nullptr};
    PFN_viOpen open{nullptr};
    PFN_viClose close{nullptr};
    PFN_viWrite write{nullptr};
    PFN_viRead read{nullptr};
    PFN_viSetAttribute setAttribute{nullptr};
    PFN_viFindRsrc findRsrc{nullptr};
    PFN_viFindNext findNext{nullptr};

    bool load(std::string* error) {
        if (module) return true;
        module = LoadLibraryA("visa64.dll");
        if (!module) module = LoadLibraryA("visa32.dll");
        if (!module) {
            if (error) *error = "NI-VISA runtime (visa64.dll) is not installed";
            return false;
        }
        auto get = [&](const char* name) { return GetProcAddress(module, name); };
        openDefaultRM = reinterpret_cast<PFN_viOpenDefaultRM>(get("viOpenDefaultRM"));
        open = reinterpret_cast<PFN_viOpen>(get("viOpen"));
        close = reinterpret_cast<PFN_viClose>(get("viClose"));
        write = reinterpret_cast<PFN_viWrite>(get("viWrite"));
        read = reinterpret_cast<PFN_viRead>(get("viRead"));
        setAttribute = reinterpret_cast<PFN_viSetAttribute>(get("viSetAttribute"));
        findRsrc = reinterpret_cast<PFN_viFindRsrc>(get("viFindRsrc"));
        findNext = reinterpret_cast<PFN_viFindNext>(get("viFindNext"));
        if (!openDefaultRM || !open || !close || !write || !read || !setAttribute || !findRsrc ||
            !findNext) {
            if (error) *error = "NI-VISA runtime is missing required entry points";
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }
};

VisaApi& visa() {
    static VisaApi api;
    return api;
}

class VisaScpiTransport final : public IScpiTransport {
public:
    ~VisaScpiTransport() override { close(); }

    bool open(const std::string& resource, std::string* error) override {
        close();
        auto& api = visa();
        if (!api.load(error)) return false;
        if (api.openDefaultRM(&rm_) < 0) {
            rm_ = kViNull;
            if (error) *error = "viOpenDefaultRM failed";
            return false;
        }
        std::string target = resource;
        if (target.empty() || target == "auto") {
            // First USB instrument the resource manager knows about.
            ViFindList list = kViNull;
            ViUInt32 count = 0;
            char name[256] = {0};
            if (api.findRsrc(rm_, "USB?*INSTR", &list, &count, name) < 0 || count == 0) {
                if (error) *error = "no USB instrument found by NI-VISA (is the SSG on USB?)";
                close();
                return false;
            }
            target = name;
            api.close(list);
        }
        if (api.open(rm_, target.c_str(), 0, 2000, &session_) < 0) {
            session_ = kViNull;
            if (error) *error = "viOpen " + target + " failed";
            close();
            return false;
        }
        api.setAttribute(session_, kViAttrTermcharEn, 1);
        api.setAttribute(session_, kViAttrTermchar, '\n');
        description_ = "usb " + target;
        return true;
    }

    void close() override {
        auto& api = visa();
        if (session_ != kViNull && api.close) api.close(session_);
        if (rm_ != kViNull && api.close) api.close(rm_);
        session_ = kViNull;
        rm_ = kViNull;
    }

    bool isOpen() const override { return session_ != kViNull; }

    bool write(std::string_view command, std::string* error) override {
        if (!isOpen()) {
            if (error) *error = "not open";
            return false;
        }
        std::string line(command);
        line.push_back('\n');
        ViUInt32 written = 0;
        const ViStatus st = visa().write(session_, reinterpret_cast<const unsigned char*>(line.data()),
                                         static_cast<ViUInt32>(line.size()), &written);
        if (st < 0 || written != line.size()) {
            if (error) *error = "viWrite failed (status " + std::to_string(st) + ")";
            return false;
        }
        return true;
    }

    bool readLine(std::string& reply, std::chrono::milliseconds timeout, std::string* error) override {
        reply.clear();
        if (!isOpen()) {
            if (error) *error = "not open";
            return false;
        }
        auto& api = visa();
        api.setAttribute(session_, kViAttrTmoValue, static_cast<ViAttrState>(timeout.count()));
        unsigned char buf[4096];
        for (;;) {
            ViUInt32 got = 0;
            const ViStatus st = api.read(session_, buf, sizeof(buf), &got);
            if (st < 0) {
                if (error) *error = "viRead failed (status " + std::to_string(st) + ")";
                return false;
            }
            reply.append(reinterpret_cast<const char*>(buf), got);
            // VI_SUCCESS_MAX_CNT: buffer filled, message continues.
            if (st != kViSuccessMaxCnt) break;
        }
        while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
        return true;
    }

    std::string describe() const override { return description_; }

private:
    ViSession rm_{kViNull};
    ViSession session_{kViNull};
    std::string description_{"usb"};
};

} // namespace

std::unique_ptr<IScpiTransport> makeUsbtmcScpiTransport() {
    return std::make_unique<VisaScpiTransport>();
}

} // namespace backend::services::scpi
