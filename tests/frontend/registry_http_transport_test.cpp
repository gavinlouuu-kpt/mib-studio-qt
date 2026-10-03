// Qt registry transport (#398 M1): runs on a plain std::thread like the
// registry worker, refuses non-HTTPS, and a request stalled in the TLS
// handshake ends on its timeout or promptly on cancellation (status 0).
#include "frontend/system/RegistryHttpTransport.h"

#include "support/assert.h"
#include "support/watchdog.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono;
using backend::profiles::RegistryHttpRequest;
using backend::profiles::RegistryHttpResponse;

namespace {
// Runs the transport on its own std::thread (Qt adopts it), as the worker does.
RegistryHttpResponse postOnWorkerThread(const RegistryHttpRequest& request,
                                        milliseconds* took = nullptr) {
    RegistryHttpResponse out;
    const auto started = steady_clock::now();
    std::thread worker([&] { out = mib::frontend::makeQtRegistryHttpTransport()(request); });
    worker.join();
    if (took) *took = duration_cast<milliseconds>(steady_clock::now() - started);
    return out;
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    mib::test::Watchdog watchdog(30);

    watchdog.mark("plain http refused");
    RegistryHttpRequest plain;
    plain.url = "http://127.0.0.1:1/rest/v1/rpc/x";
    MIB_EXPECT(postOnWorkerThread(plain).status == 0, "non-HTTPS URL is never contacted");

    // Accepts TCP (kernel backlog) but never answers the TLS ClientHello.
    QTcpServer silent;
    MIB_REQUIRE(silent.listen(QHostAddress::LocalHost), "listen");
    const std::string url = "https://127.0.0.1:" + std::to_string(silent.serverPort()) +
                            "/rest/v1/rpc/registry_list_projects";

    watchdog.mark("timeout");
    RegistryHttpRequest stalled;
    stalled.url = url;
    stalled.body = "{}";
    stalled.headers = {{"apikey", "sb_publishable_test"}};
    stalled.timeoutMs = 300;
    milliseconds took{};
    MIB_EXPECT(postOnWorkerThread(stalled, &took).status == 0, "stalled request fails");
    MIB_EXPECT(took < seconds(5), "timeout bounds the request: " + std::to_string(took.count()));

    watchdog.mark("cancel");
    std::atomic<bool> cancel{false};
    RegistryHttpRequest cancellable = stalled;
    cancellable.timeoutMs = 20000;
    cancellable.cancelled = [&] { return cancel.load(); };
    std::thread trigger([&] {
        std::this_thread::sleep_for(milliseconds(200));
        cancel = true;
    });
    MIB_EXPECT(postOnWorkerThread(cancellable, &took).status == 0, "cancelled request fails");
    trigger.join();
    MIB_EXPECT(took < seconds(5),
               "cancellation aborts well before the 20 s timeout: " + std::to_string(took.count()));

    return mib::test::exitCode();
}
