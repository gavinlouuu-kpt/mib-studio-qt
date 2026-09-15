#include "backend/nanopositioner/INanopositionerBackend.h"
#include "backend/nanopositioner/oeabt/OeabtProtocol.h"
#include "backend/nanopositioner/oeabt/SerialTransport.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace backend::nanopositioner::oeabt;

namespace {

std::string frame(std::string payload) {
    std::string response{"\xff\x1f\x30\x60", 4};
    response += std::move(payload);
    response += "\x03\r\n";
    return response;
}

std::string responseFor(const std::string& command) {
    if (command == "/1&\r")
        return "hal_pwm_io_set_vol_input_mode 0\n" + frame("Oeabt pzt controller");
    if (command == "/1?aVtmR\r") return frame("100000,0,0,0");
    if (command == "/1?aSR\r") return frame("60,0,0,0");
    if (command == "/1?et1R\r") return frame("1");
    if (command == "/1?et2R\r") return frame("1");
    if (command == "/1?aVtR\r") return frame("25000,0,0,0");
    return frame("") + "hal_pwm_io_set_vol_input_mode 0\n";
}

} // namespace

int main(int argc, char** argv) {

    mib::test::Watchdog watchdog(10);
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    MIB_REQUIRE(master >= 0, "create PTY master");
    MIB_REQUIRE(::grantpt(master) == 0 && ::unlockpt(master) == 0, "grant and unlock PTY");
    const char* slaveName = ::ptsname(master);
    MIB_REQUIRE(slaveName != nullptr, "resolve PTY slave path");

    std::atomic<bool> running{true};
    std::atomic<bool> emulatorOk{true};
    std::thread emulator([&] {
        std::string command;
        while (running.load()) {
            pollfd descriptor{master, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (ready <= 0 || !(descriptor.revents & POLLIN)) continue;

            char buffer[128];
            const auto count = ::read(master, buffer, sizeof(buffer));
            if (count <= 0) continue;
            command.append(buffer, static_cast<std::size_t>(count));
            const auto terminator = command.find('\r');
            if (terminator == std::string::npos) continue;

            const std::string complete = command.substr(0, terminator + 1);
            command.erase(0, terminator + 1);
            const std::string response = responseFor(complete);
            const auto midpoint = response.size() / 2;
            const auto firstWrite = ::write(master, response.data(), midpoint);
            if (firstWrite != static_cast<ssize_t>(midpoint)) {
                emulatorOk.store(false);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto secondSize = response.size() - midpoint;
            const auto secondWrite = ::write(master, response.data() + midpoint, secondSize);
            if (secondWrite != static_cast<ssize_t>(secondSize)) {
                emulatorOk.store(false);
                return;
            }
        }
    });

    SerialTransport transport(slaveName);
    const auto opened = transport.open();
    MIB_REQUIRE(static_cast<bool>(opened), "open native serial port on PTY");
    ControllerSession session(transport);

    MIB_REQUIRE(static_cast<bool>(session.identify()),
                "identity works over real native serial transport");
    const auto capabilities = session.readCapabilities();
    MIB_REQUIRE(static_cast<bool>(capabilities), "capabilities work over fragmented serial reads");
    MIB_EXPECT(capabilities.value().maxVoltage == VoltageMv{100000},
               "native transport preserves framed maximum voltage");
    const auto voltage = session.readVoltage();
    MIB_REQUIRE(static_cast<bool>(voltage), "voltage works over native serial transport");
    MIB_EXPECT(voltage.value() == VoltageMv{25000}, "native transport returns expected voltage");
    MIB_EXPECT(static_cast<bool>(session.setVoltage(VoltageMv{30000})),
               "mode debug output does not obscure voltage acknowledgement");
    watchdog.mark("serial transactions complete");

    transport.close();

    backend::nanopositioner::Endpoint endpoint;
    endpoint.backend = backend::nanopositioner::BackendKind::Oeabt;
    endpoint.systemPath = slaveName;
    endpoint.displayName = "PTY OEABT";
    endpoint.knownOeabtCandidate = true;
    auto backend = backend::nanopositioner::createNanopositionerBackend(
        backend::nanopositioner::BackendKind::Oeabt);
    std::string backendError;
    MIB_REQUIRE(backend && backend->connect(endpoint, backendError),
                "serialized OEABT backend connects over PTY: " + backendError);
    double backendVoltage = 0.0;
    MIB_REQUIRE(backend->readVoltage(backendVoltage, backendError),
                "serialized OEABT backend reads voltage: " + backendError);
    MIB_EXPECT(backendVoltage == 25.0, "serialized backend preserves millivolt conversion");
    std::atomic<bool> concurrentReadsOk{true};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            for (int n = 0; n < 20; ++n) {
                double value = 0;
                std::string error;
                if (!backend->readVoltage(value, error) || value != 25.0)
                    concurrentReadsOk.store(false);
            }
        });
    }
    for (auto& reader : readers)
        reader.join();
    MIB_EXPECT(concurrentReadsOk.load(), "concurrent callers preserve whole serial transactions");
    backend->disconnect();
    backend.reset();
    watchdog.mark("serialized backend disconnected");

    running.store(false);
    emulator.join();
    ::close(master);
    MIB_EXPECT(emulatorOk.load(), "PTY emulator writes complete frames");
    watchdog.mark("emulator joined");
    return mib::test::exitCode();
}
