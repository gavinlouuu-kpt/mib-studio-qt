#include "backend/services/AutofocusService.h"
#include "support/assert.h"
#include "support/watchdog.h"
#include <atomic>
#include <memory>
#include <spdlog/spdlog.h>
#include <thread>

int main() {
    mib::test::Watchdog watchdog(20);
    spdlog::set_level(spdlog::level::off);
    backend::services::AutofocusService service;
    std::atomic<bool> start{false};
    std::thread emitter([&] {
        while (!start.load())
            std::this_thread::yield();
        for (int i = 0; i < 10000; ++i)
            service.connect(0, 0, 0);
    });
    start.store(true);
    for (int i = 0; i < 10000; ++i) {
        auto count = std::make_shared<std::atomic<int>>(0);
        service.setStatusCallback([count](const std::string&) { ++*count; });
    }
    emitter.join();
    bool invoked = false;
    service.setStatusCallback([&](const std::string&) {
        invoked = true;
        service.setStatusCallback({});
    });
    service.connect(0, 0, 0);
    MIB_REQUIRE(invoked, "callback can unregister itself");
    return mib::test::exitCode();
}
