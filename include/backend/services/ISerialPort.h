// UI-neutral serial transport for the Modbus RTU services (syringe pumps,
// pulse generator, shared RS485 bus). Replaces the direct QSerialPort
// dependency so the backend links no Qt (epic #246). Payloads are byte
// vectors (ModbusRtu.h is vector-based), so this contract is Qt-free.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace backend::services {

// Serial line settings that identify a bus configuration. Two clients may
// share one adapter only when every field matches.
struct SerialSettings {
    int baudRate{9600};
    int dataBits{8};   // 5..8
    char parity{'N'};  // 'N' none, 'E' even, 'O' odd
    int stopBits{1};   // 1 or 2

    bool operator==(const SerialSettings& o) const
    {
        return baudRate == o.baudRate && dataBits == o.dataBits &&
               parity == o.parity && stopBits == o.stopBits;
    }
    bool operator!=(const SerialSettings& o) const { return !(*this == o); }
};

class ISerialPort {
public:
    virtual ~ISerialPort() = default;

    // Open COM<comPort> at baudRate with 8N1 / no flow control. Returns false
    // on failure; lastError() describes why.
    virtual bool open(int comPort, int baudRate) = 0;

    // Open a system port name ("COM3", "\\\\.\\COM12", "ttyUSB0", "/dev/pts/4")
    // with explicit line settings. The default maps "COM<n>" onto open(); the
    // platform ports override it with the full settings.
    virtual bool openNamed(const std::string& systemName, const SerialSettings& settings)
    {
        std::string digits;
        for (auto it = systemName.rbegin(); it != systemName.rend() && *it >= '0' && *it <= '9'; ++it) {
            digits.insert(digits.begin(), *it);
        }
        if (digits.empty()) return false;
        return open(std::stoi(digits), settings.baudRate);
    }

    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Write all bytes. Returns the number written, or -1 on error.
    virtual int write(const std::vector<uint8_t>& data) = 0;

    // Block until queued output has been transmitted, or timeoutMs elapses.
    virtual bool waitForBytesWritten(int timeoutMs) = 0;
    // Block until at least one byte is available to read, or timeoutMs elapses.
    virtual bool waitForReadyRead(int timeoutMs) = 0;
    // Return everything currently available (possibly empty).
    virtual std::vector<uint8_t> readAll() = 0;

    virtual std::string lastError() const = 0;
    // OS error code of the last failed open (GetLastError / errno); 0 when
    // unknown. Lets the bus layer classify "busy" without parsing text.
    virtual int lastSystemError() const { return 0; }
};

using SerialPortFactory = std::function<std::unique_ptr<ISerialPort>()>;

// Construct the platform serial port (POSIX termios or Win32), selected at
// build time by which SerialPort*.cpp is compiled.
std::unique_ptr<ISerialPort> makePlatformSerialPort();

// Test seam: open an explicit device path (e.g. a pty slave) instead of a
// COM-number mapping. POSIX builds return an opened port; other platforms
// return nullptr. Used by the loopback test to exercise the real transport.
std::unique_ptr<ISerialPort> makeSerialPortForPathForTesting(const std::string& devicePath,
                                                             int baudRate);

// One enumerated serial port with the USB identity fields (when the platform
// exposes them) that survive a device-node rename across reboots/replug.
struct SerialPortInfo {
    std::string systemName;     // "ttyUSB0", "COM3"
    std::string systemLocation; // "/dev/ttyUSB0", "\\\\.\\COM3"
    std::string description;
    std::string manufacturer;
    std::string serialNumber;
    uint16_t vendorId{0};
    uint16_t productId{0};
};

// Qt-free enumeration (Win32 SetupAPI / Linux sysfs). An explicit call each
// time, no caching, so a Refresh action always reflects hot-plug state.
std::vector<SerialPortInfo> enumerateSerialPorts();

} // namespace backend::services
