#include "backend/services/NanopositionerDiscovery.h"
#include "support/assert.h"
#include "support/watchdog.h"

using namespace backend::services;
namespace np = backend::services::nanopositioner;

int main() {
    mib::test::Watchdog watchdog;
    np::Endpoint adapter;
    adapter.systemPath = "COM6";
    adapter.backend = np::Vendor::Coremor;
    adapter.vendorId = 0x1a86;
    np::Endpoint stage = adapter;
    stage.systemPath = "COM7";
    stage.backend = np::Vendor::Oeabt;
    int probes = 0;
    auto found = np::discover({adapter, stage, stage}, [&](const np::Endpoint& port) {
        ++probes;
        return port.systemPath == "COM7";
    });
    MIB_EXPECT(found.size() == 2 && probes == 2, "deduplicate adapters before probing");
    MIB_EXPECT(found[0].identifiedVendors.empty(), "USB manufacturer does not identify instrument");
    auto selected = np::uniqueMatch(found);
    MIB_EXPECT(selected && selected->port.systemPath == "COM7", "unique validated device selected");
    found[0].identifiedVendors.push_back(np::Vendor::Coremor);
    MIB_EXPECT(np::uniqueMatch(found) == nullptr, "multiple devices require selection");
    found = np::discover({adapter}, {});
    MIB_EXPECT(np::uniqueMatch(found) == nullptr, "missing protocol cannot identify a device");
    MIB_EXPECT(found.size() == 1, "unidentified equipment remains in inventory");
    found[0].identifiedVendors = {np::Vendor::Coremor, np::Vendor::Oeabt};
    MIB_EXPECT(np::uniqueMatch(found) == nullptr, "conflicting vendor identities are ambiguous");
    watchdog.mark("independent discovery snapshots");
    std::atomic<bool> isolated{true};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&, i]() {
            for (int round = 0; round < 100; ++round) {
                auto snapshot = np::discover({adapter, stage}, [i](const np::Endpoint& port) {
                    return port.systemPath == (i % 2 ? "COM6" : "COM7");
                });
                const auto* match = np::uniqueMatch(snapshot);
                if (!match || match->port.systemPath != (i % 2 ? "COM6" : "COM7")) isolated = false;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    MIB_EXPECT(isolated.load(), "concurrent scans retain independent identities");
    return mib::test::exitCode();
}
