// crash_reporter_segv_test
//
// A hardware fault (null write on a worker thread) must leave a dump that
// carries the faulting context and a sidecar that names the fault. Before
// 2026-09-08 the SIGSEGV path wrote the dump without EXCEPTION_POINTERS:
// the dump's exception stream was the dump writer's own breakpoint and
// `!analyze -v` showed nothing (55 unusable dumps on the bench). The parent
// reads the dump's exception stream back with dbghelp and checks the code,
// and checks the JSON sidecar's "crash" object (code, module, build id).

#include "backend/services/CrashReporter.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

namespace fs = std::filesystem;
using CrashReporter = backend::services::CrashReporter;

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "     \
                      << #cond << "\n";                                     \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static int runCrashChild(const fs::path& crashDir) {
    CrashReporter::Config cfg;
    cfg.crashDir = crashDir;
    cfg.databaseDir = crashDir / "sentry-db";
    cfg.uploadPendingOnStart = false;
    cfg.installSignalHandlers = true;
    cfg.installTerminateHandler = true;
    cfg.installQtMessageHandler = false;
    CrashReporter::init(cfg);

    std::thread t([] { CrashReporter::triggerCrashForTesting(CrashReporter::FaultKind::NullDeref); });
    t.join();
    return 0;  // unreachable
}

static bool findArtifact(const fs::path& dir, const std::string& suffix, fs::path* found = nullptr) {
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.size() >= suffix.size() && fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (found) *found = entry.path();
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
// Read the exception stream of a minidump: code + faulting thread id.
static bool readDumpException(const fs::path& dump, unsigned long* code, unsigned long* threadId,
                              unsigned long long* address) {
    HANDLE file = ::CreateFileA(dump.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HANDLE mapping = ::CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) { ::CloseHandle(file); return false; }
    void* base = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    bool ok = false;
    if (base) {
        MINIDUMP_DIRECTORY* dir = nullptr;
        void* stream = nullptr;
        ULONG size = 0;
        if (::MiniDumpReadDumpStream(base, ExceptionStream, &dir, &stream, &size) && stream &&
            size >= sizeof(MINIDUMP_EXCEPTION_STREAM)) {
            const auto* es = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
            *code = es->ExceptionRecord.ExceptionCode;
            *threadId = es->ThreadId;
            *address = es->ExceptionRecord.ExceptionAddress;
            ok = true;
        }
        ::UnmapViewOfFile(base);
    }
    ::CloseHandle(mapping);
    ::CloseHandle(file);
    return ok;
}
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MIB_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define MIB_UNDER_SANITIZER 1
#endif
#endif

int main(int argc, char* argv[]) {
#ifdef MIB_UNDER_SANITIZER
    // ASan takes SIGSEGV for itself (the app handler never runs) and TSan
    // reports the handler's allocations as signal-unsafe; the dump/sidecar
    // contract is verified in the plain lanes. Exit 77 = CTest skip.
    (void)argc; (void)argv;
    std::cout << "crash_reporter_segv_test: skipped under a sanitizer\n";
    return 77;
#endif
    if (argc >= 3 && std::string(argv[1]) == "--crash-child") {
        return runCrashChild(fs::path(argv[2]));
    }

    std::cout << "crash_reporter_segv_test\n";
    auto dir = fs::temp_directory_path() / "crash_reporter_segv_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    std::string cmd = "\"" + std::string(argv[0]) + "\" --crash-child \"" + dir.string() + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = std::system(cmd.c_str());
    CHECK(rc != 0);

    fs::path json;
    CHECK(findArtifact(dir, "-sigsegv.json", &json) || findArtifact(dir, "-seh.json", &json));
#ifdef _WIN32
    // The "crash" object and the kept PDB are the Windows path (dbghelp /
    // CodeView); POSIX builds write the plain state sidecar.
    if (!json.empty()) {
        std::ifstream f(json);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        CHECK(text.find("\"crash\":{") != std::string::npos);
        CHECK(text.find("\"code\":\"0xC0000005\"") != std::string::npos);
        CHECK(text.find("\"module\":\"crash_reporter_segv_test.exe\"") != std::string::npos);
        CHECK(text.find("\"access\":\"write\"") != std::string::npos);
        CHECK(text.find("\"exe_build_id\":\"\"") == std::string::npos); // build id known
        if (g_failures) std::cerr << "sidecar: " << text.substr(text.find("\"crash\""), 400) << "\n";
    }

    fs::path dump;
    CHECK(findArtifact(dir, "-sigsegv.dmp", &dump) || findArtifact(dir, "-seh.dmp", &dump));
    if (!dump.empty()) {
        unsigned long code = 0, tid = 0;
        unsigned long long address = 0;
        CHECK(readDumpException(dump, &code, &tid, &address));
        CHECK(code == 0xC0000005UL);  // the access violation, not the dump writer's breakpoint
        CHECK(tid != 0);
        CHECK(address != 0);
        std::cout << "dump exception 0x" << std::hex << code << " at 0x" << address << " thread " << std::dec << tid << "\n";
    }
    // The PDB of the crashing binary is kept next to the crash dir under its build id.
    bool keptPdb = false;
    for (auto& entry : fs::recursive_directory_iterator(dir.parent_path() / "symbols", ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().filename() == "crash_reporter_segv_test.pdb") keptPdb = true;
    }
    CHECK(keptPdb);
#endif

    if (g_failures != 0) {
        std::cerr << "child exit code: " << rc << "\ncrash dir contents:\n";
        for (auto& entry : fs::directory_iterator(dir, ec)) std::cerr << "  " << entry.path().filename().string() << "\n";
    }
    fs::remove_all(dir, ec);
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}
