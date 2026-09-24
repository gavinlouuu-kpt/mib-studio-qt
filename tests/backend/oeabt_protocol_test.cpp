#include "backend/nanopositioner/oeabt/OeabtProtocol.h"

#include "support/assert.h"

#include <deque>
#include <string>
#include <vector>

using namespace backend::nanopositioner::oeabt;

namespace {

std::string frame(std::string payload, unsigned char status = 0x60,
                  unsigned char startCharacter = 0x1f) {
    std::string response;
    response.push_back(static_cast<char>(0xff));
    response.push_back(static_cast<char>(startCharacter));
    response.push_back(static_cast<char>(0x30));
    response.push_back(static_cast<char>(status));
    response += std::move(payload);
    response += "\x03\r\n";
    return response;
}

class FakeTransport final : public IByteTransport {
public:
    Result<void> clear() override {
        ++clearCount;
        return Result<void>::success();
    }

    Result<void> write(std::string_view bytes) override {
        writes.emplace_back(bytes);
        return Result<void>::success();
    }

    Result<std::string> readUntil(char delimiter, std::chrono::milliseconds timeout) override {
        (void)delimiter;
        (void)timeout;
        if (responses.empty()) {
            return Result<std::string>::failure(Error{ErrorCode::Timeout, "fake timeout", {}});
        }
        auto result = std::move(responses.front());
        responses.pop_front();
        return result;
    }

    void respond(std::string value) {
        responses.push_back(Result<std::string>::success(std::move(value)));
    }

    void timeout() {
        responses.push_back(
            Result<std::string>::failure(Error{ErrorCode::Timeout, "fake timeout", {}}));
    }

    std::deque<Result<std::string>> responses;
    std::vector<std::string> writes;
    int clearCount{0};
};

} // namespace

int main() {
    {
        FakeTransport transport;
        transport.respond(frame("Oeabt pzt controller"));
        ControllerSession session(transport);
        const auto identity = session.identify();
        MIB_EXPECT(static_cast<bool>(identity), "identity marker accepted");
        MIB_EXPECT(transport.writes.size() == 1 && transport.writes[0] == "/1&\r",
                   "identity first uses the vendor application's read-only command");
    }

    {
        FakeTransport transport;
        transport.timeout();
        transport.respond(frame("Oeabt pzt controller V0.5.0", 0x60, 0x2f));
        ControllerSession session(transport);
        const auto identity = session.identify();
        MIB_EXPECT(static_cast<bool>(identity), "documented identity fallback is accepted");
        MIB_EXPECT(transport.writes.size() == 2 && transport.writes[0] == "/1&\r" &&
                       transport.writes[1] == "/1&R\r",
                   "identity falls back to the command manual's explicit Run form");
    }

    {
        FakeTransport transport;
        transport.respond(frame("24500,0,0,0"));
        ControllerSession session(transport);
        const auto voltage = session.readVoltage();
        MIB_REQUIRE(static_cast<bool>(voltage), "voltage response parses");
        MIB_EXPECT(voltage.value() == VoltageMv{24500}, "voltage remains integer mV");
        MIB_EXPECT(transport.writes[0] == "/1?aVtR\r", "voltage query bytes match vendor app");
    }

    {
        FakeTransport transport;
        transport.respond(frame("100000,0,0,0"));
        transport.respond(frame("60,0,0,0"));
        transport.respond(frame("1"));
        transport.respond(frame("0"));
        ControllerSession session(transport);
        const auto capabilities = session.readCapabilities();
        MIB_REQUIRE(static_cast<bool>(capabilities), "capability responses parse");
        MIB_EXPECT(capabilities.value().maxVoltage == VoltageMv{100000},
                   "maximum voltage parsed as mV");
        MIB_EXPECT(capabilities.value().maximumStrokeRaw == "60",
                   "unverified stroke unit remains raw");
        MIB_EXPECT(capabilities.value().analogControl && !capabilities.value().pwmControl,
                   "mode capabilities parsed");
        MIB_EXPECT(transport.writes[0] == "/1?aVtmR\r" && transport.writes[1] == "/1?aSR\r" &&
                       transport.writes[2] == "/1?et1R\r" && transport.writes[3] == "/1?et2R\r",
                   "capability query sequence is exact");
    }

    {
        FakeTransport transport;
        transport.respond(frame("100000,0,0,0"));
        transport.respond(frame("62,0,0,0"));
        transport.respond(frame("0,0,0,0"));
        transport.respond(frame("1,0,0,0"));
        ControllerSession session(transport);
        const auto capabilities = session.readCapabilities();
        MIB_EXPECT(static_cast<bool>(capabilities), "V0.5.4 four-axis capabilities parse");
        if (capabilities) {
            MIB_EXPECT(!capabilities.value().analogControl && capabilities.value().pwmControl,
                       "V0.5.4 capability flags use axis one");
        }
    }

    {
        FakeTransport transport;
        transport.respond(frame("0,0,0,0"));
        transport.respond(frame("0,0,0,0"));
        ControllerSession session(transport);
        MIB_REQUIRE(static_cast<bool>(session.setSafetyLimits(VoltageMv{0}, VoltageMv{100000})),
                    "valid safety limits accepted");
        const auto write = session.setVoltage(VoltageMv{50123});
        MIB_REQUIRE(static_cast<bool>(write), "in-range voltage write succeeds");
        MIB_EXPECT(transport.writes.size() == 2 && transport.writes[0] == "/1aM1et0R\r" &&
                       transport.writes[1] == "/1aM1Vt50123R\r",
                   "voltage write explicitly enters manual mode and uses integer mV");

        const auto writeCount = transport.writes.size();
        const auto rejected = session.setVoltage(VoltageMv{100001});
        MIB_EXPECT(!rejected && rejected.error().code == ErrorCode::OutOfRange,
                   "out-of-range voltage is rejected");
        MIB_EXPECT(transport.writes.size() == writeCount, "range rejection emits no serial bytes");
    }

    {
        FakeTransport transport;
        transport.timeout();
        transport.timeout();
        ControllerSession session(transport);
        const auto identity = session.identify();
        MIB_EXPECT(!identity && identity.error().code == ErrorCode::Timeout,
                   "read-only identity timeout is surfaced");
        MIB_EXPECT(transport.writes.size() == 2, "read-only identity tries two safe forms");
        MIB_EXPECT(transport.writes[0] == "/1&\r" && transport.writes[1] == "/1&R\r",
                   "identity timeout tries both published command forms");
    }

    {
        const auto shortFrame = ControllerSession::parsePayload("tiny");
        MIB_EXPECT(!shortFrame && shortFrame.error().code == ErrorCode::ProtocolError,
                   "short response framing is rejected");
    }

    {
        const auto malformed = ControllerSession::parsePayload("ABCDpayloadZ\r\n");
        MIB_EXPECT(!malformed && malformed.error().code == ErrorCode::ProtocolError,
                   "incorrect response sentinels are rejected");

        const auto busy = ControllerSession::parsePayload(frame("", 0x40));
        MIB_EXPECT(!busy && busy.error().code == ErrorCode::Busy,
                   "not-ready device status is surfaced as busy");

        const auto invalidCommand = ControllerSession::parsePayload(frame("", 0x62));
        MIB_EXPECT(!invalidCommand && invalidCommand.error().code == ErrorCode::ProtocolError,
                   "device error status is rejected");
    }

    return mib::test::exitCode();
}
