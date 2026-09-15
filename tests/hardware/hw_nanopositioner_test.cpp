// hw_nanopositioner_test  (LABEL: hardware) — runs only against a real stage.
//
// Set MIB_TEST_NANOPOSITIONER_PORT to a Linux serial path/persistent ID for
// OEABT, or to a COM number for CoreMOR. Optional:
// MIB_TEST_NANOPOSITIONER_BACKEND=oeabt|coremor (inferred when absent),
// MIB_TEST_NANOPOSITIONER_BAUD (default 115200), and
// MIB_TEST_NANOPOSITIONER_ADDR (default 1). This test is read-only.

#include "backend/services/AutofocusService.h"

#include "support/assert.h"
#include "support/hardware.h"

#include <QCoreApplication>

#include <cstdio>
#include <cstdlib>
#include <string>

using backend::services::AutofocusService;

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const std::string port = mib::test::requireDeviceEnv("MIB_TEST_NANOPOSITIONER_PORT");
    const int baud = mib::test::envInt("MIB_TEST_NANOPOSITIONER_BAUD", 115200);
    const int addr = mib::test::envInt("MIB_TEST_NANOPOSITIONER_ADDR", 1);

    backend::nanopositioner::Endpoint endpoint;
    endpoint.persistentId = port;
    endpoint.systemPath = port;
    endpoint.displayName = port;
    endpoint.coremorBaudRate = baud;
    endpoint.coremorAddress = static_cast<unsigned char>(addr);

    const char* backendEnv = std::getenv("MIB_TEST_NANOPOSITIONER_BACKEND");
    const std::string backendName = backendEnv ? backendEnv : "";
    if (backendName == "coremor" || (backendName.empty() && port.find('/') == std::string::npos)) {
        endpoint.backend = backend::nanopositioner::BackendKind::Coremor;
        const std::string digits = port.rfind("COM", 0) == 0 ? port.substr(3) : port;
        endpoint.coremorPort = std::atoi(digits.c_str());
    } else {
        endpoint.backend = backend::nanopositioner::BackendKind::Oeabt;
        endpoint.knownOeabtCandidate = true;
    }

    MIB_EXPECT(AutofocusService::probeEndpoint(endpoint),
               "endpoint identifies and returns a plausible voltage");

    AutofocusService af;
    MIB_REQUIRE(af.connect(endpoint), "connect to nanopositioner");
    MIB_EXPECT(af.isConnected(), "nanopositioner reports connected");
    af.disconnect();
    MIB_EXPECT(!af.isConnected(), "nanopositioner disconnects cleanly");

    if (mib::test::exitCode() == 0) std::printf("nanopositioner hardware OK\n");
    return mib::test::exitCode();
}
