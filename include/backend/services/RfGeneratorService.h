#pragma once

#include "backend/recording/RfGeneratorProvenance.h"
#include "backend/services/ScpiTransport.h"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace backend::services {

// Control/provenance link to the SIGLENT SSG3021X RF generator that turns the
// sort TTL edge into the RF burst (pulse modulation, external trigger mode).
//
// Responsibilities — readback first, never write unasked:
//   - connect(): open the SCPI link, verify *IDN? names an SSG3000X-family
//     unit (anything else is refused as IncompatibleDevice), read the state.
//   - readState(): the generator's actual configuration (trigger mode, delay,
//     width, RF on, ...) for provenance and the readiness gate.
//   - preflightForSorting(): pure check of a state against what a sorting
//     run needs (RF on, pulse modulation on, external trigger, internal pulse
//     source, non-zero width); each failure names the SCPI/menu remedy.
//   - applySortWindow(): the one write path — set trigger delay and pulse
//     width, then read both back and refuse silently-clamped values.
//
// Timing never goes over this link (see ScpiTransport.h).
class RfGeneratorService {
public:
    struct Config {
        bool enabled{false};
        std::string transport{"usb"}; // "usb" | "lan"
        std::string resource{"auto"}; // see IScpiTransport::open
        int timeoutMs{1000};
    };

    enum class LinkError {
        None,
        NotConfigured,
        TransportUnavailable, // no such transport on this platform
        OpenFailed,
        Timeout,
        ProtocolError,      // reply not parseable / unexpected
        IncompatibleDevice, // *IDN? is not an SSG3000X-family unit
        NotConnected,
        VerifyMismatch,     // write succeeded but readback differs
    };

    struct PreflightIssue {
        std::string gate;    // "rf.output", "rf.pulseMod", "rf.triggerMode", ...
        std::string message; // what is wrong
        std::string remedy;  // menu path / SCPI to fix it
        bool blocking{true}; // false = warning (e.g. no PULSE OUT for loopback)
    };

    using State = backend::recording::RfGeneratorProvenance;

    explicit RfGeneratorService(scpi::ScpiTransportFactory factory = {});
    ~RfGeneratorService();

    void setConfig(const Config& config);
    Config config() const;

    // Open + identify + first readback. Idempotent while connected.
    bool connect();
    void disconnect();
    bool isConnected() const;
    // connect() with a back-off: a failed attempt is not retried for
    // `retryAfter` (readiness polls must not hammer a missing instrument).
    bool ensureConnected(std::chrono::milliseconds retryAfter = std::chrono::seconds(5));

    bool readState(State& out);
    // Last successful readback (empty identity when none).
    State lastState() const;

    static std::vector<PreflightIssue> preflightForSorting(const State& state);

    // Write :PULM:DELay / :PULM:WIDTh (seconds) and verify by readback.
    bool applySortWindow(double triggerDelayS, double pulseWidthS);

    LinkError lastError() const;
    std::string lastErrorMessage() const;
    static const char* toString(LinkError e);

    // Pure helpers (unit-tested without a link).
    static bool identityLooksLikeSsg(const std::string& idn);
    static bool parseBool(const std::string& reply, bool& out);
    static bool parseNumber(const std::string& reply, double& out);
    // "SSG3021X" from "Siglent Technologies,SSG3021X,SSG3XBAX1R0001,3.1.21".
    static std::string modelFromIdentity(const std::string& idn);

private:
    bool query(const std::string& command, std::string& reply);
    bool send(const std::string& command);
    bool readStateLocked(State& out);
    void fail(LinkError e, const std::string& message);

    scpi::ScpiTransportFactory factory_;
    mutable std::recursive_mutex mutex_;
    Config config_;
    std::unique_ptr<scpi::IScpiTransport> link_;
    bool connected_{false};
    State lastState_;
    LinkError lastError_{LinkError::None};
    std::string lastErrorMessage_;
    std::chrono::steady_clock::time_point lastFailedConnect_{};
    bool hasFailedConnect_{false};
};

} // namespace backend::services
