#pragma once

#include "backend/services/SerialBus.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>


namespace backend::services {

/**
 * Controls a Zhongsheng (中盛科技) pulse frequency & duty-cycle output module
 * over RS485 Modbus RTU. The module generates the TTL pulse train used as the
 * camera's external acquisition-trigger source (NOT the sort-output pulse —
 * see TriggerService for that).
 *
 * Device identity is (physical bus, serial settings, Modbus slave address):
 * the service is a client of a shared ModbusBusSession (one QSerialPort per
 * adapter, owned by SerialBusManager), so several generators — and other
 * Modbus devices — can live on one RS485 adapter at different addresses.
 * Channel is a per-device setting below that identity.
 *
 * Protocol (vendor manual 脉冲频率与占空比输出系列 V2.0):
 *  - Modbus RTU, default address 1, 9600 8N1.
 *  - Per channel N (0-based): frequency as u32 = Hz*100 across holding
 *    registers 3N (high word) and 3N+1 (low word); duty as u16 = %*100 at
 *    register 3N+2.
 *  - Output range 400 Hz – 40 kHz, duty 0–100 %, resolution 0.01 Hz / 0.01 %.
 *  - There is no run/stop register: the pulse train is gated by writing duty
 *    0 % (line idles low) and restoring the configured duty to resume.
 */
class PulseGeneratorService {
public:
    static constexpr int CHANNEL_COUNT = 4;
    static constexpr double MIN_FREQUENCY_HZ = 400.0;
    static constexpr double MAX_FREQUENCY_HZ = 40000.0;

    using SerialSettings = serialbus::SerialSettings;

    // Why the last connect/write failed, mapped from the bus layer so the GUI
    // can distinguish "port unavailable" from "bus timeout" from "collision".
    enum class LinkError {
        None,
        PortUnavailable,
        PortBusy,
        Timeout,
        CrcFrameError,
        ModbusException,
        AddressCollision,
        IncompatibleDevice,
        NotConnected,
        WriteFailed
    };
    static const char* toString(LinkError error);

    struct Config {
        std::string portName;         // system port name: "ttyUSB0", "COM3"
        SerialSettings serial{};  // 9600 8N1 module factory default
        uint8_t modbusAddress{1};
    };

    struct ChannelState {
        double frequencyHz{0.0};    // last written/read frequency
        double dutyPercent{0.0};    // configured duty (restored on enable)
        bool outputEnabled{false};  // false = duty 0 written to the device
    };

    struct Status {
        bool connected{false};
        LinkError lastError{LinkError::None};
        std::array<ChannelState, CHANNEL_COUNT> channels{};
    };

    // One probed address from a bus scan.
    struct ScanHit {
        uint8_t address{0};
        enum class Kind {
            PulseGenerator, // answered the FC03 identity read with the expected shape
            ModbusDevice,   // valid Modbus response, but not a pulse generator
            Error           // corrupt/inconsistent response — possible collision
        } kind{Kind::Error};
        // PulseGenerator only: raw frequency register (Hz×100) per channel
        // from the identity read. The module stores 0 for a channel that has
        // never been set (observed on the rig: ch1 5000 Hz, ch2–4 0 Hz), so
        // automatic discovery keys on the requested channel, not on all four.
        std::array<uint32_t, CHANNEL_COUNT> channelFrequencyRaw{};
    };

    explicit PulseGeneratorService(serialbus::SerialBusManager& busManager);
    ~PulseGeneratorService();

    PulseGeneratorService(const PulseGeneratorService&) = delete;
    PulseGeneratorService& operator=(const PulseGeneratorService&) = delete;

    // Connection management. connect() acquires the shared bus session for
    // portName (system port name, never a synthesized "COMn") and verifies the
    // addressed device by reading back all channel registers, seeding
    // ChannelState from the hardware (a channel reads as enabled when its duty
    // is non-zero). Never writes during connect.
    bool connect(const std::string& portName, const SerialSettings& settings, uint8_t modbusAddress);
    bool connect(const std::string& portName, int baudRate, uint8_t modbusAddress);
    void disconnect();
    bool isConnected() const;
    LinkError lastError() const;

    // Read-only bus discovery: FC03 identity read per address in [from, to] on
    // the given port. Never emits a write function code, so a generator that
    // is already pulsing keeps pulsing and unknown devices are left untouched.
    // Checks `cancel` between addresses; silent addresses are omitted.
    // Synchronous — run it off the GUI thread and use `cancel` to abort.
    // When the port itself cannot be acquired, returns empty and reports why
    // through `error` (so callers can distinguish that from a silent bus).
    std::vector<ScanHit> scanBus(const std::string& portName, const SerialSettings& settings,
                                 uint8_t from, uint8_t to, const std::atomic<bool>& cancel,
                                 int perAddressTimeoutMs = 250, LinkError* error = nullptr);

    // Read-only discovery for `port: "auto"` profiles: probes every USB serial
    // adapter in `ports` at the configured address/settings and resolves
    // config.portName. A port counts only when the reply has the generator
    // shape AND the requested 0-based `channel` holds a non-zero in-range
    // frequency (identityChannelConfigured) AND a read of the syringe pump's
    // syringe-volume register answers zero (a dLSP pump left channel-enabled
    // at the same address would otherwise pass the shape test). A generator
    // whose requested channel was never set, or an unrelated device serving
    // zeroed registers, is reported, never adopted. Exactly one such port is
    // required; none or several fail with an operator-facing message that names
    // busy, unconfigured and non-generator ports. Never writes to any device.
    bool discoverLiveView(Config& config, int channel,
                          const std::vector<serialbus::PortInfo>& ports,
                          std::string* error = nullptr);

    // Control. Channel is 0-based [0, CHANNEL_COUNT). Values are clamped to
    // the module's range before writing. setDutyCycle stores the configured
    // duty and writes it only while the channel is enabled; setOutputEnabled
    // gates the pulse train (duty 0 = off) without losing the configured duty.
    bool setFrequency(int channel, double hz);
    bool setDutyCycle(int channel, double percent);
    bool setOutputEnabled(int channel, bool on);

    // Exclusive coordinated capture ownership; manual writes/disconnect are
    // rejected until endLiveView. Values validated, never silently clamped.
    bool beginLiveView(const Config& config, int channel, double hz, double duty,
                       const void* owner = nullptr);
    bool enableLiveView(const void* owner = nullptr);
    bool endLiveView(const void* owner = nullptr);
    bool liveViewOwned() const;

    Status getStatus() const;
    Config getConfig() const;

    // --- Pure encoding helpers (unit-testable without a serial port) -------
    static double clampFrequency(double hz);
    static double clampDuty(double percent);
    static uint32_t frequencyToRegisterValue(double hz);  // round(Hz * 100)
    static uint16_t dutyToRegisterValue(double percent);  // round(% * 100)
    // FC16 write of [freq high word, freq low word] at register 3*channel.
    static std::vector<uint8_t> buildFrequencyFrame(uint8_t addr, int channel, double hz);
    // FC06 write of the duty register 3*channel + 2.
    static std::vector<uint8_t> buildDutyFrame(uint8_t addr, int channel, double percent);
    // True when the raw bytes of the 12-register identity read are plausible
    // for this module: per channel, frequency raw is 0 or within
    // [400 Hz, 40 kHz]×100 and duty raw ≤ 100%×100. Guards against adopting
    // (and later writing into) an unrelated Modbus device that merely serves
    // 12 holding registers at address 0.
    static bool identityLooksLikeGenerator(const std::vector<uint8_t>& identityData);
    // Automatic-adoption form: identityLooksLikeGenerator AND the 0-based
    // `channel` reads a non-zero (hence in-range) frequency. The module keeps
    // 0 Hz for channels never set, so a fresh module or a foreign device
    // serving zeros is excluded while a rig whose channel was set once passes.
    static bool identityChannelConfigured(const std::vector<uint8_t>& identityData, int channel);
    // Raw frequency register (Hz×100) of `channel` from an identity read.
    static uint32_t identityFrequencyRaw(const std::vector<uint8_t>& identityData, int channel);
    // Holding register the dLSP syringe pump uses for syringe volume (1–9999).
    // The generator answers 0 for any register outside its map (observed).
    static constexpr uint16_t SYRINGE_PUMP_VOLUME_REGISTER = 0x0061;

private:
    bool writeFrame(const std::vector<uint8_t>& request);
    // One FC03 read on an arbitrary port (read-only, shares an open session).
    // Returns false when the port cannot be opened or the device does not
    // answer with data; `value` receives the register on success.
    bool readRegisterOnPort(const std::string& portName, const SerialSettings& settings,
                            uint8_t address, uint16_t reg, uint16_t& value);
    static bool validChannel(int channel);
    static LinkError mapBusError(serialbus::BusError error);

    serialbus::SerialBusManager& busManager_;
    std::shared_ptr<serialbus::ModbusBusSession> bus_;
    Config config_;
    Status status_;
    mutable std::recursive_mutex mutex_;
    bool verifyLiveView(double duty);
    const void* liveViewOwner_{nullptr};
    bool liveViewOwned_{false};
    int liveViewChannel_{0};
};

} // namespace backend::services
