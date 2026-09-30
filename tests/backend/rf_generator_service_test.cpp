// rf_generator_service_test
//
// RfGeneratorService (SIGLENT SSG3021X sort generator over SCPI):
//   - connect identifies first: a non-SSG *IDN? is refused before any PULM
//     command is sent; a matching one is followed by a full readback;
//   - readState parses every field from the documented reply formats
//     (1|0 booleans, long-form enumerations, floats in s/Hz/dBm);
//   - preflightForSorting names every blocking issue with a remedy and the
//     PULSE OUT loopback as a warning only;
//   - applySortWindow writes delay + width, waits for *OPC?, reads both back
//     and refuses a silently clamped value (VerifyMismatch);
//   - faults: timeout, garbage reply, link dropped mid-query, unknown
//     transport, ensureConnected back-off;
//   - the real LAN transport against a loopback TCP server running the same
//     fake instrument (line framing, port parsing, closed-by-peer).

#include "backend/services/RfGeneratorService.h"
#include "backend/services/ScpiTransport.h"

#include "support/assert.h"
#include "support/fake_ssg.h"
#include "support/watchdog.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using backend::services::RfGeneratorService;
using backend::services::scpi::IScpiTransport;
using mib::test::FakeSsg;
using mib::test::FakeSsgTransport;
#ifndef _WIN32
using mib::test::LoopbackSsgServer;
#endif
using LinkError = RfGeneratorService::LinkError;

namespace {

bool near(double a, double b, double tol = 1e-12) { return std::fabs(a - b) <= tol; }

RfGeneratorService::Config cfg(const std::string& transport = "fake", const std::string& resource = "auto") {
    RfGeneratorService::Config c;
    c.enabled = true;
    c.transport = transport;
    c.resource = resource;
    c.timeoutMs = 200;
    return c;
}

} // namespace

int main()
{
    mib::test::Watchdog wd(60);

    // ---- pure helpers ----
    {
        wd.mark("helpers");
        MIB_EXPECT(RfGeneratorService::identityLooksLikeSsg("Siglent Technologies,SSG3021X,SSG3XBAX1R0001,3.1.21"),
                   "SSG3021X identity accepted");
        MIB_EXPECT(RfGeneratorService::identityLooksLikeSsg("Siglent Technologies,SSG5040X,SSG5XCAQ1R0001,4.0.0.1"),
                   "SSG5000X family accepted (same PULM set)");
        MIB_EXPECT(!RfGeneratorService::identityLooksLikeSsg("Siglent Technologies,SDG1032X,SDG1XCAQ2R0001,1.01"),
                   "a Siglent function generator is not an SSG");
        MIB_EXPECT(!RfGeneratorService::identityLooksLikeSsg("Rigol Technologies,DSG3060,DSG3A000000,00.01"),
                   "another vendor refused");
        MIB_EXPECT(RfGeneratorService::modelFromIdentity("Siglent Technologies,SSG3021X,S,3.1") == "SSG3021X",
                   "model parsed");
        bool b = false;
        MIB_EXPECT(RfGeneratorService::parseBool("1", b) && b, "1 -> true");
        MIB_EXPECT(RfGeneratorService::parseBool("OFF", b) && !b, "OFF -> false");
        MIB_EXPECT(!RfGeneratorService::parseBool("maybe", b), "junk boolean rejected");
        double d = 0;
        MIB_EXPECT(RfGeneratorService::parseNumber("0.03", d) && near(d, 0.03), "seconds parsed");
        MIB_EXPECT(RfGeneratorService::parseNumber("2000000 Hz", d) && near(d, 2e6), "trailing unit tolerated");
        MIB_EXPECT(!RfGeneratorService::parseNumber("?!#", d), "garbage number rejected");
        MIB_EXPECT(!RfGeneratorService::parseNumber("", d), "empty number rejected");
    }

    // ---- connect: identify first, refuse strangers, read back ----
    {
        wd.mark("identify");
        FakeSsg ssg;
        ssg.idn = "Siglent Technologies,SDG1032X,SDG1XCAQ2R0001,1.01";
        auto factory = [&](const std::string&) { return std::unique_ptr<IScpiTransport>(new FakeSsgTransport(ssg, nullptr)); };
        RfGeneratorService svc(factory);
        MIB_EXPECT(!svc.connect(), "connect without config refused");
        MIB_EXPECT(svc.lastError() == LinkError::NotConfigured, "NotConfigured");
        svc.setConfig(cfg());
        MIB_EXPECT(!svc.connect(), "non-SSG refused");
        MIB_EXPECT(svc.lastError() == LinkError::IncompatibleDevice, "IncompatibleDevice");
        MIB_EXPECT(!ssg.sawPulmCommand(), "no PULM command reached a foreign instrument");
        MIB_EXPECT(!svc.isConnected(), "not connected after refusal");

        ssg.idn = "Siglent Technologies,SSG3021X,SSG3XBAX1R0001,3.1.21";
        svc.setConfig(cfg()); // resets the back-off
        MIB_REQUIRE(svc.connect(), "SSG accepted");
        MIB_EXPECT(svc.isConnected(), "connected");
        const auto s = svc.lastState();
        MIB_EXPECT(s.identity == ssg.idn, "identity stored");
        MIB_EXPECT(s.rfOutputOn && s.pulseModOn && s.pulseOutOn, "booleans read");
        MIB_EXPECT(s.pulseSource == "INTernal" && s.pulseMode == "SINGle" && s.triggerMode == "EXTernal" &&
                       s.triggerSlope == "POSitive",
                   "enumerations read verbatim");
        MIB_EXPECT(near(s.triggerDelayS, 140e-9, 1e-15) && near(s.pulseWidthS, 50e-6, 1e-12) &&
                       near(s.pulsePeriodS, 10e-3, 1e-12),
                   "delay/width/period in seconds");
        MIB_EXPECT(near(s.frequencyHz, 1.2e9, 1e-3) && near(s.powerDbm, -3.0), "frequency/power read");
        MIB_EXPECT(s.sampledHostUs != 0 && s.link == "fake", "sample stamp + link description");
        MIB_EXPECT(svc.connect(), "connect is idempotent");
        MIB_EXPECT(RfGeneratorService::preflightForSorting(s).empty(), "armed instrument passes preflight");
        svc.disconnect();
        MIB_EXPECT(!svc.isConnected(), "disconnected");
    }

    // ---- preflight names each problem ----
    {
        wd.mark("preflight");
        RfGeneratorService::State s;
        s.rfOutputOn = false;
        s.pulseModOn = false;
        s.triggerMode = "AUTO";
        s.pulseSource = "EXTernal";
        s.pulseWidthS = 0.0;
        s.pulseOutOn = false;
        const auto issues = RfGeneratorService::preflightForSorting(s);
        std::map<std::string, bool> byGate;
        for (const auto& i : issues) {
            byGate[i.gate] = i.blocking;
            MIB_EXPECT(!i.message.empty() && !i.remedy.empty(), "issue carries message + remedy: " + i.gate);
        }
        MIB_EXPECT(byGate.size() == 6, "six issues reported");
        MIB_EXPECT(byGate["rf.output"] && byGate["rf.pulseMod"] && byGate["rf.triggerMode"] &&
                       byGate["rf.pulseSource"] && byGate["rf.pulseWidth"],
                   "five blocking");
        MIB_EXPECT(byGate.count("rf.pulseOut") && !byGate["rf.pulseOut"], "PULSE OUT off is a warning only");
        // Short-form enumerations are accepted ("EXT" == "EXTernal").
        RfGeneratorService::State ok;
        ok.rfOutputOn = ok.pulseModOn = ok.pulseOutOn = true;
        ok.triggerMode = "EXT";
        ok.pulseSource = "INT";
        ok.pulseWidthS = 1e-6;
        MIB_EXPECT(RfGeneratorService::preflightForSorting(ok).empty(), "short-form enumerations accepted");
    }

    // ---- applySortWindow: write, *OPC?, verify; clamp is refused ----
    {
        wd.mark("apply");
        FakeSsg ssg;
        auto factory = [&](const std::string&) { return std::unique_ptr<IScpiTransport>(new FakeSsgTransport(ssg, nullptr)); };
        RfGeneratorService svc(factory);
        svc.setConfig(cfg());
        MIB_REQUIRE(svc.connect(), "connect");
        MIB_EXPECT(svc.applySortWindow(2.5e-6, 20e-6), "window applied");
        MIB_EXPECT(near(ssg.delayS, 2.5e-6, 1e-15) && near(ssg.widthS, 20e-6, 1e-15), "instrument holds the values");
        MIB_EXPECT(near(svc.lastState().triggerDelayS, 2.5e-6, 1e-15), "cached state updated from readback");
        bool sawOpc = false;
        {
            std::lock_guard<std::mutex> lk(ssg.m);
            for (const auto& l : ssg.log) if (l == "*OPC?") sawOpc = true;
        }
        MIB_EXPECT(sawOpc, "*OPC? sent between write and readback");
        MIB_EXPECT(!svc.applySortWindow(1e-9, 20e-6), "out-of-range delay refused before any write");
        MIB_EXPECT(svc.lastError() == LinkError::ProtocolError, "range error reported");
        ssg.clampWidthTo = 300.0; // instrument silently clamps
        MIB_EXPECT(!svc.applySortWindow(2.5e-6, 20e-6), "clamped readback refused");
        MIB_EXPECT(svc.lastError() == LinkError::VerifyMismatch, "VerifyMismatch");
    }

    // ---- faults ----
    {
        wd.mark("faults");
        FakeSsg ssg;
        std::atomic<bool> drop{false};
        auto factory = [&](const std::string& kind) -> std::unique_ptr<IScpiTransport> {
            if (kind == "bogus") return nullptr;
            return std::unique_ptr<IScpiTransport>(new FakeSsgTransport(ssg, &drop));
        };
        RfGeneratorService svc(factory);
        svc.setConfig(cfg("bogus"));
        MIB_EXPECT(!svc.connect() && svc.lastError() == LinkError::TransportUnavailable, "unknown transport");
        svc.setConfig(cfg("fake", "missing"));
        MIB_EXPECT(!svc.connect() && svc.lastError() == LinkError::OpenFailed, "open failure reported");
        // Back-off: a second attempt inside the window is not made.
        {
            std::lock_guard<std::mutex> lk(ssg.m);
            ssg.log.clear();
        }
        MIB_EXPECT(!svc.ensureConnected(std::chrono::seconds(30)), "ensureConnected honours back-off");
        {
            std::lock_guard<std::mutex> lk(ssg.m);
            MIB_EXPECT(ssg.log.empty(), "no traffic during back-off");
        }
        MIB_EXPECT(!svc.ensureConnected(std::chrono::milliseconds(0)), "zero back-off retries (still missing)");

        svc.setConfig(cfg());
        ssg.silent = true;
        MIB_EXPECT(!svc.connect() && svc.lastError() == LinkError::Timeout, "silent instrument -> Timeout");
        ssg.silent = false;
        ssg.garbage = true;
        svc.setConfig(cfg());
        MIB_EXPECT(!svc.connect() && svc.lastError() == LinkError::IncompatibleDevice,
                   "garbage *IDN? is an incompatible device");
        ssg.garbage = false;
        svc.setConfig(cfg());
        MIB_REQUIRE(svc.connect(), "connect after faults cleared");
        ssg.garbage = true;
        RfGeneratorService::State s;
        MIB_EXPECT(!svc.readState(s) && svc.lastError() == LinkError::ProtocolError, "garbage reply -> ProtocolError");
        MIB_EXPECT(svc.isConnected(), "a protocol error keeps the link");
        ssg.garbage = false;
        drop.store(true);
        MIB_EXPECT(!svc.readState(s), "dropped link fails the readback");
        MIB_EXPECT(!svc.isConnected(), "dropped link marks disconnected");
        drop.store(false);
        MIB_EXPECT(svc.ensureConnected(std::chrono::milliseconds(0)), "reconnects once the link is back");
        MIB_EXPECT(svc.readState(s) && s.identity == ssg.idn, "readback after reconnect");
    }

#ifndef _WIN32
    // ---- real LAN transport against a loopback instrument ----
    {
        wd.mark("lan");
        FakeSsg ssg;
        LoopbackSsgServer server(ssg);
        MIB_REQUIRE(server.start(), "loopback server up");
        RfGeneratorService svc; // default factory: real transports
        svc.setConfig(cfg("lan", "127.0.0.1:" + std::to_string(server.port())));
        MIB_REQUIRE(svc.connect(), "connect over TCP");
        const auto s = svc.lastState();
        MIB_EXPECT(s.identity == ssg.idn, "identity over TCP (split replies reassembled)");
        MIB_EXPECT(s.link == "lan 127.0.0.1:" + std::to_string(server.port()), "link description");
        MIB_EXPECT(near(s.pulseWidthS, 50e-6, 1e-12), "numeric field over TCP");
        MIB_EXPECT(svc.applySortWindow(1e-6, 30e-6) && near(ssg.widthS, 30e-6, 1e-15), "write+verify over TCP");
        // Peer closes: the next query fails loudly and the service reconnects
        // on demand (a new TCP connection).
        server.dropClient();
        RfGeneratorService::State again;
        MIB_EXPECT(!svc.readState(again), "closed-by-peer fails the query");
        MIB_EXPECT(!svc.isConnected(), "closed-by-peer marks disconnected");
        MIB_EXPECT(svc.ensureConnected(std::chrono::milliseconds(0)), "reconnects to the instrument");
        MIB_EXPECT(server.connections.load() == 2, "second TCP connection made");
        svc.disconnect();
        // Unreachable port: OpenFailed, quickly.
        RfGeneratorService none;
        none.setConfig(cfg("lan", "127.0.0.1:1"));
        MIB_EXPECT(!none.connect() && none.lastError() == LinkError::OpenFailed, "refused port -> OpenFailed");
        RfGeneratorService bad;
        bad.setConfig(cfg("lan", ""));
        MIB_EXPECT(!bad.connect() && bad.lastError() == LinkError::OpenFailed, "empty host -> OpenFailed");
        // Unplugged / mis-addressed instrument: a non-routable address must
        // fail within the transport's connect bound, not the OS timeout (a
        // readiness poll blocks on this).
        RfGeneratorService unplugged;
        unplugged.setConfig(cfg("lan", "10.255.255.1:5025"));
        const auto t0 = std::chrono::steady_clock::now();
        const bool connected = unplugged.connect();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        MIB_EXPECT(!connected && unplugged.lastError() == LinkError::OpenFailed, "unreachable -> OpenFailed");
        MIB_EXPECT(elapsed < std::chrono::milliseconds(4000),
                   "unreachable host fails within the connect bound (" + std::to_string(elapsed.count()) + " ms)");
    }
#endif

    return mib::test::exitCode();
}
