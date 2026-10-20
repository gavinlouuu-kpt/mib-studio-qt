#include "backend/app/Tools.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <fstream>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Kernel32.lib")
#pragma comment(lib, "Advapi32.lib")
#else
#include <time.h>
#endif

namespace backend {

uint64_t Tools::getTimestamp() {
#ifdef _WIN32
    LARGE_INTEGER freqc, nowc;
    uint64_t freq, now;

    QueryPerformanceFrequency(&freqc);
    QueryPerformanceCounter(&nowc);
    freq = static_cast<uint64_t>(freqc.QuadPart);
    now = static_cast<uint64_t>(nowc.QuadPart);

    return (now * 1000000ULL) / freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (static_cast<uint64_t>(ts.tv_sec) * 1000000ULL) + (ts.tv_nsec / 1000ULL);
#endif
}

void Tools::log(const std::string& msg) {
    SPDLOG_INFO("{}", msg);
}

#ifdef _WIN32
static inline double bytesToMB(SIZE_T bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
#endif

double Tools::getProcessMemoryMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc),
                             sizeof(pmc))) {
        // Prefer PrivateUsage when available, fallback to WorkingSetSize
        const SIZE_T bytes = pmc.PrivateUsage ? pmc.PrivateUsage : pmc.WorkingSetSize;
        return bytesToMB(bytes);
    }
    return 0.0;
#else
    return 0.0;
#endif
}

double Tools::getPeakProcessMemoryMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        // PeakWorkingSetSize is commonly used; PeakPagefileUsage could also be considered
        return bytesToMB(pmc.PeakWorkingSetSize);
    }
    return 0.0;
#else
    return 0.0;
#endif
}

uint64_t Tools::getAvailableSystemRAMBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memStatus)) {
        return static_cast<uint64_t>(memStatus.ullAvailPhys);
    }
    return 0;
#elif defined(__linux__)
    std::ifstream input("/proc/meminfo");
    std::string key, unit;
    uint64_t kib=0;
    while(input >> key >> kib >> unit) {
        if(key=="MemAvailable:" && unit=="kB" && kib<=std::numeric_limits<uint64_t>::max()/1024)
            return kib*1024;
        input.ignore(std::numeric_limits<std::streamsize>::max(),'\n');
    }
    return 0;
#else
    return 0;
#endif
}

std::vector<int> Tools::availableComPortNumbers() {
#ifdef _WIN32
    std::vector<int> ports;
    HKEY hKey = nullptr;
    const wchar_t* subkey = L"HARDWARE\\DEVICEMAP\\SERIALCOMM";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return ports;
    }
    wchar_t valueName[256];
    wchar_t valueData[64];
    DWORD valueNameSize;
    DWORD valueDataSize;
    DWORD valueType;
    for (DWORD i = 0;; ++i) {
        valueNameSize = static_cast<DWORD>(std::size(valueName));
        valueDataSize = static_cast<DWORD>(sizeof(valueData));
        if (RegEnumValueW(hKey, i, valueName, &valueNameSize, nullptr, &valueType,
                          reinterpret_cast<LPBYTE>(valueData), &valueDataSize) != ERROR_SUCCESS) {
            break;
        }
        if ((valueType != REG_SZ && valueType != REG_EXPAND_SZ) || valueDataSize < 4 * sizeof(wchar_t)) {
            continue;
        }
        const size_t valueChars = std::min<size_t>(valueDataSize / sizeof(wchar_t), std::size(valueData) - 1);
        valueData[valueChars] = L'\0';
        if (valueData[0] == L'C' && valueData[1] == L'O' && valueData[2] == L'M') {
            wchar_t* end = nullptr;
            const long num = std::wcstol(valueData + 3, &end, 10);
            if (end != valueData + 3 && num >= 1 && num <= 4096) {
                ports.push_back(static_cast<int>(num));
            }
        }
    }
    RegCloseKey(hKey);
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
#else
    return {};
#endif
}

} // namespace backend
