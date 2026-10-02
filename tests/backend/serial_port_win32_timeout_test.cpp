// Exercise the real Win32 transport against a stalled driver, without a COM device.
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include "support/assert.h"
#include "support/watchdog.h"

namespace {
bool stalled = true;
bool driverError = false;
int closes = 0;
HANDLE WINAPI fakeCreateFile(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    return reinterpret_cast<HANDLE>(42);
}
BOOL WINAPI fakeGetState(HANDLE, LPDCB) {
    return TRUE;
}
BOOL WINAPI fakeSetState(HANDLE, LPDCB) {
    return TRUE;
}
BOOL WINAPI fakeTimeouts(HANDLE, LPCOMMTIMEOUTS) {
    return TRUE;
}
BOOL WINAPI fakePurge(HANDLE, DWORD) {
    return TRUE;
}
BOOL WINAPI fakeClose(HANDLE) {
    ++closes;
    return TRUE;
}
BOOL WINAPI fakeClearError(HANDLE, LPDWORD errors, LPCOMSTAT stat) {
    *errors = 0;
    stat->cbOutQue = stalled ? 10 : 0;
    if (driverError) SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    return !driverError;
}
BOOL WINAPI fakeFlush(HANDLE) {
    // FlushFileBuffers has no timeout. Model a device that never drains.
    for (;;)
        Sleep(100);
}
} // namespace
#define CreateFileA fakeCreateFile
#define GetCommState fakeGetState
#define SetCommState fakeSetState
#define SetCommTimeouts fakeTimeouts
#define PurgeComm fakePurge
#define CloseHandle fakeClose
#define ClearCommError fakeClearError
#define FlushFileBuffers fakeFlush
#include "../../src/backend/services/SerialPortWin32.cpp"

int main() {
    mib::test::Watchdog watchdog(5);
    auto port = backend::services::makePlatformSerialPort();
    MIB_REQUIRE(port->open(7, 115200), "fake port opens");
    watchdog.mark("stalled output must time out");
    MIB_EXPECT(!port->waitForBytesWritten(20), "stalled driver returns failure");
    MIB_EXPECT(port->lastError().find("timed out") != std::string::npos, "timeout is actionable");
    stalled = false;
    MIB_EXPECT(port->waitForBytesWritten(20), "drained output succeeds");
    driverError = true;
    MIB_EXPECT(!port->waitForBytesWritten(20), "unplugged driver returns failure");
    MIB_EXPECT(port->lastSystemError() == ERROR_DEVICE_NOT_CONNECTED, "driver error preserved");
    port->close();
    MIB_EXPECT(closes == 1, "port released after failures");
    MIB_EXPECT(!port->waitForBytesWritten(0), "closed port fails");
    return mib::test::exitCode();
}
