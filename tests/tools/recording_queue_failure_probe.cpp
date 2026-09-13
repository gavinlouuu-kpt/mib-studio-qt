// Diagnostic for v1.1.1 queue failure boundaries; not a production change.
#include "backend/recording/HdfWriteQueue.h"
#include "support/watchdog.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
using backend::recording::HdfWriteQueue;
int main() {
    mib::test::Watchdog watchdog(10);
    std::atomic<bool> entered{false}, release{false}, stopped{false};
    std::atomic<int> written{0}, errors{0};
    HdfWriteQueue<int> q(3, [&](const int&) {
        entered = true;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++written; return true;
    }, [&](const std::string&) { ++errors; });
    int accepted = q.submit(0);
    while (!entered.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (int i=1; i<=3; ++i) accepted += q.submit(int(i));
    const bool overflowAccepted = q.submit(4);
    bool stopOk = true;
    std::thread stopper([&] { stopOk = q.flushAndStop(); stopped = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool waitsForWriter = !stopped.load();
    release = true;
    stopper.join();
    std::printf("overflow accepted=%d rejected=%d written=%d unwritten=%d errors=%d stopOk=%d stopWaitsForWriter=%d\n",
                accepted, !overflowAccepted, written.load(), accepted-written.load(), errors.load(), stopOk, waitsForWriter);
    int cleanWritten = 0;
    HdfWriteQueue<int> clean(3, [&](const int&) { ++cleanWritten; return true; }, [](const std::string&) {});
    clean.submit(1);
    const bool firstStop = clean.flushAndStop();
    const bool afterStopAccepted = clean.submit(2);
    const bool secondStop = clean.flushAndStop();
    std::printf("post-stop firstStop=%d acceptedAfterStop=%d secondStop=%d written=%d\n", firstStop, afterStopAccepted, secondStop, cleanWritten);
    return 0;
}
