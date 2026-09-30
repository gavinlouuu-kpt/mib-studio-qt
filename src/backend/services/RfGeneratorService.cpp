#include "backend/services/RfGeneratorService.h"

#include "backend/app/Tools.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace backend::services {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// SCPI enumerations come back in long form ("EXTernal"); the instrument
// accepts either, and the comparison here is on the canonical upper form.
bool enumIs(const std::string& reply, const char* longForm) {
    const std::string r = upper(trim(reply));
    const std::string l = upper(longForm);
    if (r == l) return true;
    // Short form = the upper-case prefix of the long form ("EXT").
    std::string shortForm;
    for (char c : std::string(longForm)) {
        if (std::isupper(static_cast<unsigned char>(c))) shortForm.push_back(c);
    }
    return r == shortForm;
}

std::string formatSeconds(double s) {
    // Send in the unit that keeps the mantissa readable; the instrument
    // parses "ns|us|ms|s" suffixes (programming guide 3.4.8).
    char buf[64];
    if (s < 1e-6) std::snprintf(buf, sizeof(buf), "%.3f ns", s * 1e9);
    else if (s < 1e-3) std::snprintf(buf, sizeof(buf), "%.3f us", s * 1e6);
    else if (s < 1.0) std::snprintf(buf, sizeof(buf), "%.6f ms", s * 1e3);
    else std::snprintf(buf, sizeof(buf), "%.9f s", s);
    return buf;
}

// Readback compares at the instrument's resolution (10 ns on delay/width).
bool sameSeconds(double a, double b) { return std::fabs(a - b) <= 1.5e-8; }

} // namespace

RfGeneratorService::RfGeneratorService(scpi::ScpiTransportFactory factory)
    : factory_(factory ? std::move(factory) : scpi::ScpiTransportFactory(&scpi::makeScpiTransport)) {}

RfGeneratorService::~RfGeneratorService() { disconnect(); }

void RfGeneratorService::setConfig(const Config& config) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    const bool linkChanged = config.transport != config_.transport || config.resource != config_.resource;
    config_ = config;
    hasFailedConnect_ = false; // a new configuration deserves a fresh attempt
    if (linkChanged && connected_) disconnect();
}

RfGeneratorService::Config RfGeneratorService::config() const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return config_;
}

const char* RfGeneratorService::toString(LinkError e) {
    switch (e) {
    case LinkError::None: return "none";
    case LinkError::NotConfigured: return "notConfigured";
    case LinkError::TransportUnavailable: return "transportUnavailable";
    case LinkError::OpenFailed: return "openFailed";
    case LinkError::Timeout: return "timeout";
    case LinkError::ProtocolError: return "protocolError";
    case LinkError::IncompatibleDevice: return "incompatibleDevice";
    case LinkError::NotConnected: return "notConnected";
    case LinkError::VerifyMismatch: return "verifyMismatch";
    }
    return "unknown";
}

void RfGeneratorService::fail(LinkError e, const std::string& message) {
    lastError_ = e;
    lastErrorMessage_ = message;
    SPDLOG_WARN("RfGeneratorService: {} ({})", message, toString(e));
}

bool RfGeneratorService::identityLooksLikeSsg(const std::string& idn) {
    // "Siglent Technologies,SSG3021X,<serial>,<firmware>"; the SSG5000X
    // family shares the PULM command set used here.
    const std::string u = upper(idn);
    return u.find("SIGLENT") != std::string::npos && u.find(",SSG") != std::string::npos;
}

std::string RfGeneratorService::modelFromIdentity(const std::string& idn) {
    const auto first = idn.find(',');
    if (first == std::string::npos) return {};
    const auto second = idn.find(',', first + 1);
    return trim(idn.substr(first + 1, second == std::string::npos ? std::string::npos
                                                                   : second - first - 1));
}

bool RfGeneratorService::parseBool(const std::string& reply, bool& out) {
    const std::string r = upper(trim(reply));
    if (r == "1" || r == "ON") {
        out = true;
        return true;
    }
    if (r == "0" || r == "OFF") {
        out = false;
        return true;
    }
    return false;
}

bool RfGeneratorService::parseNumber(const std::string& reply, double& out) {
    const std::string r = trim(reply);
    if (r.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(r.c_str(), &end);
    if (end == r.c_str() || !std::isfinite(v)) return false;
    // Tolerate a trailing unit the guide does not promise but some firmware
    // adds ("2000000 Hz"); anything else is a protocol error.
    const std::string rest = trim(std::string(end));
    if (!rest.empty() && rest != "Hz" && rest != "s" && rest != "dBm") return false;
    out = v;
    return true;
}

bool RfGeneratorService::connect() {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (connected_) return true;
    if (!config_.enabled) {
        fail(LinkError::NotConfigured, "RF generator link is not enabled in the configuration");
        return false;
    }
    link_ = factory_(config_.transport);
    if (!link_) {
        fail(LinkError::TransportUnavailable, "unknown RF generator transport '" + config_.transport + "'");
        hasFailedConnect_ = true;
        lastFailedConnect_ = std::chrono::steady_clock::now();
        return false;
    }
    std::string why;
    if (!link_->open(config_.resource, &why)) {
        fail(why.find("not supported") != std::string::npos || why.find("not installed") != std::string::npos
                 ? LinkError::TransportUnavailable
                 : LinkError::OpenFailed,
             "cannot open RF generator link (" + config_.transport + " " + config_.resource + "): " + why);
        link_.reset();
        hasFailedConnect_ = true;
        lastFailedConnect_ = std::chrono::steady_clock::now();
        return false;
    }
    // Identify before anything else: never talk PULM to a stranger.
    std::string idn;
    if (!query("*IDN?", idn)) {
        link_->close();
        link_.reset();
        hasFailedConnect_ = true;
        lastFailedConnect_ = std::chrono::steady_clock::now();
        return false;
    }
    if (!identityLooksLikeSsg(idn)) {
        fail(LinkError::IncompatibleDevice,
             "instrument on " + link_->describe() + " is not a SIGLENT SSG: \"" + idn + "\"");
        link_->close();
        link_.reset();
        hasFailedConnect_ = true;
        lastFailedConnect_ = std::chrono::steady_clock::now();
        return false;
    }
    connected_ = true;
    hasFailedConnect_ = false;
    State state;
    state.identity = trim(idn);
    if (!readStateLocked(state)) {
        // Identified but not readable: keep the link (a later readState may
        // succeed) and report the failure.
        return false;
    }
    lastError_ = LinkError::None;
    lastErrorMessage_.clear();
    SPDLOG_INFO("RfGeneratorService: connected to {} via {} (trigger {} delay {} s, width {} s, RF {})",
                state.identity, link_->describe(), state.triggerMode, state.triggerDelayS,
                state.pulseWidthS, state.rfOutputOn ? "on" : "off");
    return true;
}

bool RfGeneratorService::ensureConnected(std::chrono::milliseconds retryAfter) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (connected_) return true;
    if (hasFailedConnect_ && std::chrono::steady_clock::now() - lastFailedConnect_ < retryAfter) {
        return false; // hold the last error; do not hammer a missing instrument
    }
    return connect();
}

void RfGeneratorService::disconnect() {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (link_) link_->close();
    link_.reset();
    connected_ = false;
}

bool RfGeneratorService::isConnected() const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return connected_;
}

bool RfGeneratorService::send(const std::string& command) {
    if (!link_ || !link_->isOpen()) {
        fail(LinkError::NotConnected, "RF generator link is not open");
        connected_ = false;
        return false;
    }
    std::string why;
    if (!link_->write(command, &why)) {
        fail(LinkError::OpenFailed, "write '" + command + "' failed: " + why);
        connected_ = false;
        return false;
    }
    return true;
}

bool RfGeneratorService::query(const std::string& command, std::string& reply) {
    if (!send(command)) return false;
    std::string why;
    if (!link_->readLine(reply, std::chrono::milliseconds(std::max(1, config_.timeoutMs)), &why)) {
        const bool timeout = why.find("timeout") != std::string::npos;
        fail(timeout ? LinkError::Timeout : LinkError::OpenFailed,
             "query '" + command + "' failed: " + why);
        if (!timeout) connected_ = false;
        return false;
    }
    reply = trim(reply);
    return true;
}

bool RfGeneratorService::readStateLocked(State& out) {
    std::string r;
    auto boolField = [&](const char* cmd, bool& dst) {
        if (!query(cmd, r)) return false;
        if (!parseBool(r, dst)) {
            fail(LinkError::ProtocolError, std::string("unexpected reply to ") + cmd + ": \"" + r + "\"");
            return false;
        }
        return true;
    };
    auto numField = [&](const char* cmd, double& dst) {
        if (!query(cmd, r)) return false;
        if (!parseNumber(r, dst)) {
            fail(LinkError::ProtocolError, std::string("unexpected reply to ") + cmd + ": \"" + r + "\"");
            return false;
        }
        return true;
    };
    auto enumField = [&](const char* cmd, std::string& dst) {
        if (!query(cmd, r)) return false;
        if (r.empty()) {
            fail(LinkError::ProtocolError, std::string("empty reply to ") + cmd);
            return false;
        }
        dst = r;
        return true;
    };
    if (out.identity.empty()) {
        if (!query("*IDN?", r)) return false;
        out.identity = r;
    }
    out.link = link_ ? link_->describe() : std::string();
    // Programming guide (SSG3000X Series, 3.3 / 3.4.3 / 3.4.4 / 3.4.8).
    if (!boolField(":OUTPut?", out.rfOutputOn)) return false;
    if (!boolField(":PULM:STATe?", out.pulseModOn)) return false;
    if (!enumField(":PULM:SOURce?", out.pulseSource)) return false;
    if (!enumField(":PULM:MODE?", out.pulseMode)) return false;
    if (!enumField(":PULM:TRIGger:MODE?", out.triggerMode)) return false;
    if (!enumField(":PULM:TRIGger:EXTernal:SLOPe?", out.triggerSlope)) return false;
    if (!numField(":PULM:DELay?", out.triggerDelayS)) return false;
    if (!numField(":PULM:WIDTh?", out.pulseWidthS)) return false;
    if (!numField(":PULM:PERiod?", out.pulsePeriodS)) return false;
    if (!boolField(":PULM:OUT:STATe?", out.pulseOutOn)) return false;
    if (!numField(":FREQuency?", out.frequencyHz)) return false;
    if (!numField(":POWer?", out.powerDbm)) return false;
    out.sampledHostUs = backend::Tools::getTimestamp();
    lastState_ = out;
    return true;
}

bool RfGeneratorService::readState(State& out) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (!connected_) {
        fail(LinkError::NotConnected, "RF generator is not connected");
        return false;
    }
    out = State{};
    return readStateLocked(out);
}

RfGeneratorService::State RfGeneratorService::lastState() const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return lastState_;
}

std::vector<RfGeneratorService::PreflightIssue>
RfGeneratorService::preflightForSorting(const State& s) {
    std::vector<PreflightIssue> issues;
    if (!s.rfOutputOn) {
        issues.push_back({"rf.output", "RF output is off: no sort burst will be emitted",
                          "RF ON/OFF key, or :OUTPut ON", true});
    }
    if (!s.pulseModOn) {
        issues.push_back({"rf.pulseMod", "pulse modulation is off: RF is continuous, not a sort burst",
                          "MOD > PULSE > Pulse State ON, or :PULM:STATe ON", true});
    }
    if (!enumIs(s.triggerMode, "EXTernal")) {
        issues.push_back({"rf.triggerMode",
                          "pulse trigger mode is " + s.triggerMode + ", not external: the sort TTL edge is ignored",
                          "MOD > PULSE > Pulse Trigger = Ext Trig, or :PULM:TRIGger:MODE EXTernal", true});
    }
    if (!enumIs(s.pulseSource, "INTernal")) {
        issues.push_back({"rf.pulseSource",
                          "pulse source is " + s.pulseSource + ": the RF envelope follows PULSE IN, not the triggered pulse",
                          "MOD > PULSE > Pulse Source = Int, or :PULM:SOURce INTernal", true});
    }
    if (!(s.pulseWidthS > 0.0)) {
        issues.push_back({"rf.pulseWidth", "pulse width is zero", ":PULM:WIDTh <value>", true});
    }
    if (!s.pulseOutOn) {
        issues.push_back({"rf.pulseOut",
                          "PULSE OUT is off: no envelope to loop back into a timestamped grabber input",
                          "MOD > PULSE > Pulse Out ON, or :PULM:OUT:STATe ON (only if the connector is free)",
                          false});
    }
    return issues;
}

bool RfGeneratorService::applySortWindow(double triggerDelayS, double pulseWidthS) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (!connected_) {
        fail(LinkError::NotConnected, "RF generator is not connected");
        return false;
    }
    if (!(triggerDelayS >= 140e-9 && triggerDelayS <= 300.0) || !(pulseWidthS >= 20e-9 && pulseWidthS <= 300.0)) {
        fail(LinkError::ProtocolError, "sort window out of instrument range (delay 140 ns..300 s, width 20 ns..300 s)");
        return false;
    }
    if (!send(":PULM:DELay " + formatSeconds(triggerDelayS))) return false;
    if (!send(":PULM:WIDTh " + formatSeconds(pulseWidthS))) return false;
    // *OPC? makes the instrument acknowledge the writes before we read back.
    std::string r;
    if (!query("*OPC?", r)) return false;
    double delay = 0.0, width = 0.0;
    if (!query(":PULM:DELay?", r) || !parseNumber(r, delay)) {
        if (lastError_ == LinkError::None) fail(LinkError::ProtocolError, "unreadable :PULM:DELay? after write");
        return false;
    }
    if (!query(":PULM:WIDTh?", r) || !parseNumber(r, width)) {
        if (lastError_ == LinkError::None) fail(LinkError::ProtocolError, "unreadable :PULM:WIDTh? after write");
        return false;
    }
    if (!sameSeconds(delay, triggerDelayS) || !sameSeconds(width, pulseWidthS)) {
        fail(LinkError::VerifyMismatch,
             "instrument holds delay " + std::to_string(delay) + " s / width " + std::to_string(width) +
                 " s after requesting " + std::to_string(triggerDelayS) + " / " + std::to_string(pulseWidthS));
        return false;
    }
    lastState_.triggerDelayS = delay;
    lastState_.pulseWidthS = width;
    lastError_ = LinkError::None;
    lastErrorMessage_.clear();
    SPDLOG_INFO("RfGeneratorService: sort window applied and verified: delay {} s, width {} s", delay, width);
    return true;
}

RfGeneratorService::LinkError RfGeneratorService::lastError() const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return lastError_;
}

std::string RfGeneratorService::lastErrorMessage() const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return lastErrorMessage_;
}

} // namespace backend::services
