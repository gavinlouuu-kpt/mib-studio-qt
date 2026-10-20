// Win32 implementation of ISerialPort. Compiled on Windows only (selected in
// src/backend/CMakeLists.txt). Qt-free.
#include "backend/services/ISerialPort.h"
#include "backend/services/SimulatedSerial.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>

namespace backend::services {

namespace {

class Win32SerialPort final : public ISerialPort {
public:
    ~Win32SerialPort() override { close(); }

    bool open(int comPort, int baudRate) override
    {
        SerialSettings s;
        s.baudRate = baudRate;
        return openNamed("COM" + std::to_string(comPort), s);
    }

    bool openNamed(const std::string& systemName, const SerialSettings& settings) override
    {
        close();
        interrupted_.store(false);
        // \\.\COMn form is required for COM10 and above.
        const std::string path = systemName.rfind("\\\\.\\", 0) == 0 ? systemName
                                                                   : "\\\\.\\" + systemName;
        handle_ = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            setLastError("CreateFile " + path);
            return false;
        }
        const int baudRate = settings.baudRate;

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!::GetCommState(handle_, &dcb)) {
            setLastError("GetCommState");
            close();
            return false;
        }
        dcb.BaudRate = static_cast<DWORD>(baudRate);
        dcb.ByteSize = static_cast<BYTE>(settings.dataBits >= 5 && settings.dataBits <= 8 ? settings.dataBits : 8);
        dcb.Parity = settings.parity == 'E' ? EVENPARITY : settings.parity == 'O' ? ODDPARITY : NOPARITY;
        dcb.StopBits = settings.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fParity = settings.parity == 'E' || settings.parity == 'O' ? TRUE : FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        if (!::SetCommState(handle_, &dcb)) {
            setLastError("SetCommState");
            close();
            return false;
        }

        // Non-blocking reads: return immediately with whatever is queued; the
        // service layer polls via waitForReadyRead().
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = 0;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = static_cast<DWORD>(kSerialWriteTimeoutMs);
        timeouts.WriteTotalTimeoutMultiplier = 0;
        if (!::SetCommTimeouts(handle_, &timeouts)) {
            setLastError("SetCommTimeouts");
            close();
            return false;
        }
        ::PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        error_.clear();
        return true;
    }

    void close() override
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (interrupted_.load()) ::PurgeComm(handle_, PURGE_TXABORT | PURGE_TXCLEAR);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool isOpen() const override { return handle_ != INVALID_HANDLE_VALUE; }

    void interrupt() override
    {
        interrupted_.store(true);
        // Aborts a WriteFile blocked on this handle (ERROR_OPERATION_ABORTED);
        // the polling waits below check the flag.
        if (handle_ != INVALID_HANDLE_VALUE) ::CancelIoEx(handle_, nullptr);
    }

    int write(const std::vector<uint8_t>& data) override
    {
        if (handle_ == INVALID_HANDLE_VALUE) {
            error_ = "write on closed port";
            return -1;
        }
        if (interrupted_.load()) { failInterrupted(); return -1; }
        DWORD written = 0;
        if (!::WriteFile(handle_, data.data(), static_cast<DWORD>(data.size()),
                         &written, nullptr)) {
            if (interrupted_.load()) { failInterrupted(); return -1; }
            setLastError("WriteFile");
            return -1;
        }
        if (written < data.size()) {
            // The COM write timeout expired with the driver queue full: drop
            // the partial frame so it is not sent late (#720).
            ::PurgeComm(handle_, PURGE_TXABORT | PURGE_TXCLEAR);
            systemError_ = ERROR_TIMEOUT;
            error_ = "Serial write timed out after " + std::to_string(kSerialWriteTimeoutMs) +
                     " ms (" + std::to_string(written) + " of " + std::to_string(data.size()) +
                     " bytes taken, discarded)";
        }
        return static_cast<int>(written);
    }

    bool waitForBytesWritten(int timeoutMs) override {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        // FlushFileBuffers ignores COM write timeouts and can hold a hardware
        // worker (and its shutdown join) forever. Poll the transmit queue.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
        for (;;) {
            if (interrupted_.load()) return failInterrupted();
            COMSTAT stat{};
            DWORD errors = 0;
            if (!::ClearCommError(handle_, &errors, &stat)) {
                setLastError("ClearCommError while waiting for serial output");
                return false;
            }
            if (stat.cbOutQue == 0) return true;
            if (std::chrono::steady_clock::now() >= deadline) {
                // Drop what was not sent, so a command the caller reports as
                // failed cannot reach the device later (#720).
                ::PurgeComm(handle_, PURGE_TXABORT | PURGE_TXCLEAR);
                systemError_ = ERROR_TIMEOUT;
                error_ = "Serial output timed out (" + std::to_string(stat.cbOutQue) +
                         " bytes not drained, discarded)";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool waitForReadyRead(int timeoutMs) override {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        // Poll the input queue up to the timeout.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (interrupted_.load()) return false;
            if (queuedBytes() > 0) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::vector<uint8_t> readAll() override {
        std::vector<uint8_t> out;
        if (handle_ == INVALID_HANDLE_VALUE) return out;
        const DWORD avail = queuedBytes();
        if (avail == 0) return out;
        out.resize(avail);
        DWORD read = 0;
        if (!::ReadFile(handle_, out.data(), avail, &read, nullptr)) {
            setLastError("ReadFile");
            out.clear();
            return out;
        }
        out.resize(read);
        return out;
    }

    std::string lastError() const override { return error_; }
    int lastSystemError() const override { return systemError_; }

private:
    DWORD queuedBytes()
    {
        COMSTAT stat{};
        DWORD errors = 0;
        if (!::ClearCommError(handle_, &errors, &stat)) {
            return 0;
        }
        return stat.cbInQue;
    }

    // interrupt() was called: drop what is queued and fail the call.
    bool failInterrupted()
    {
        ::PurgeComm(handle_, PURGE_TXABORT | PURGE_TXCLEAR);
        systemError_ = ERROR_OPERATION_ABORTED;
        error_ = "Serial port interrupted (session closing), output discarded";
        return false;
    }

    void setLastError(const std::string& what)
    {
        systemError_ = static_cast<int>(::GetLastError());
        error_ = what + " failed (GetLastError=" + std::to_string(systemError_) + ")";
    }

    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::atomic<bool> interrupted_{false};
    std::string error_;
    int systemError_{0};
};

} // namespace

std::unique_ptr<ISerialPort> makePlatformSerialPort()
{
    if (auto rig = serialsim::installed()) return rig->makePort();
    return std::make_unique<Win32SerialPort>();
}

std::unique_ptr<ISerialPort> makeSerialPortForPathForTesting(const std::string& /*devicePath*/,
                                                             int /*baudRate*/)
{
    // Path-based (pty) opening is a POSIX-only test seam.
    return nullptr;
}

// SetupAPI enumeration of the COM-port device interface class: friendly name,
// manufacturer, and the USB VID/PID parsed from the hardware id.
std::vector<SerialPortInfo> enumerateSerialPorts()
{
    if (auto rig = serialsim::installed()) return rig->enumerate();
    std::vector<SerialPortInfo> ports;
    static const GUID kComPortClass = {0x86E0D1E0L, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};
    const HDEVINFO devs = ::SetupDiGetClassDevsA(&kComPortClass, nullptr, nullptr,
                                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return ports;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(devs, i, &info); ++i) {
        SerialPortInfo p;
        const HKEY key = ::SetupDiOpenDevRegKey(devs, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            char name[64];
            DWORD size = sizeof(name);
            DWORD type = 0;
            if (::RegQueryValueExA(key, "PortName", nullptr, &type, reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS &&
                type == REG_SZ) {
                p.systemName = name;
            }
            ::RegCloseKey(key);
        }
        if (p.systemName.empty()) continue;
        p.systemLocation = "\\\\.\\" + p.systemName;
        char text[512];
        if (::SetupDiGetDeviceRegistryPropertyA(devs, &info, SPDRP_FRIENDLYNAME, nullptr,
                                                reinterpret_cast<PBYTE>(text), sizeof(text), nullptr)) {
            p.description = text;
        }
        if (::SetupDiGetDeviceRegistryPropertyA(devs, &info, SPDRP_MFG, nullptr,
                                                reinterpret_cast<PBYTE>(text), sizeof(text), nullptr)) {
            p.manufacturer = text;
        }
        if (::SetupDiGetDeviceRegistryPropertyA(devs, &info, SPDRP_HARDWAREID, nullptr,
                                                reinterpret_cast<PBYTE>(text), sizeof(text), nullptr)) {
            const std::string id(text);
            const auto vid = id.find("VID_");
            const auto pid = id.find("PID_");
            if (vid != std::string::npos && pid != std::string::npos) {
                p.vendorId = static_cast<uint16_t>(std::strtoul(id.c_str() + vid + 4, nullptr, 16));
                p.productId = static_cast<uint16_t>(std::strtoul(id.c_str() + pid + 4, nullptr, 16));
            }
        }
        char instanceId[512];
        if (::SetupDiGetDeviceInstanceIdA(devs, &info, instanceId, sizeof(instanceId), nullptr)) {
            // USB\VID_xxxx&PID_xxxx\<serial>
            const std::string inst(instanceId);
            const auto slash = inst.find_last_of('\\');
            if (slash != std::string::npos && inst.rfind("USB", 0) == 0) p.serialNumber = inst.substr(slash + 1);
        }
        ports.push_back(std::move(p));
    }
    ::SetupDiDestroyDeviceInfoList(devs);
    return ports;
}

} // namespace backend::services
