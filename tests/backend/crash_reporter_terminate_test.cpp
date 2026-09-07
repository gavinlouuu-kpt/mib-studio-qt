// Verifies the CrashReporter std::terminate path end-to-end in a child
// process: an unhandled C++ exception on a background thread must leave a
// state .json sidecar, a .txt with the exception message, and (on Windows)
// a .dmp minidump in the crash dir — with Sentry active OR inactive, since
// neither Crashpad nor the SIGABRT fallback ever captures this path (issue
// #347, gap 1).
//
// The test binary re-spawns itself with --crash-child <dir>; the child
// installs the reporter local-only (no DSN, so the shared handler code runs
// deterministically), then lets a std::runtime_error escape a std::thread.
//
// Checks use an explicit failure counter (not assert) so the test still
// verifies behavior in Release/NDEBUG builds.

#include "backend/services/CrashReporter.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

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
    // dsn left empty: local-only, so the child's behavior does not depend
    // on network or crashpad_handler presence. terminateHandler runs the
    // same code either way.
    cfg.uploadPendingOnStart = false;
    cfg.installSignalHandlers = true;
    cfg.installTerminateHandler = true;
    cfg.installQtMessageHandler = false;
    CrashReporter::init(cfg);

    // The exception escapes the thread entry point → std::terminate on the
    // background thread, never reaching main's try/catch.
    std::thread t([] {
        CrashReporter::triggerCrashForTesting(CrashReporter::FaultKind::Throw);
    });
    t.join();
    return 0;  // unreachable
}

static bool findArtifact(const fs::path& dir, const std::string& suffix,
                         fs::path* found = nullptr) {
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.size() >= suffix.size() &&
            fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (found) *found = entry.path();
            return true;
        }
    }
    return false;
}

int main(int argc, char* argv[]) {
    if (argc >= 3 && std::string(argv[1]) == "--crash-child") {
        return runCrashChild(fs::path(argv[2]));
    }

    std::cout << "crash_reporter_terminate_test\n";

    auto dir = fs::temp_directory_path() / "crash_reporter_terminate_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    std::string cmd = "\"" + std::string(argv[0]) + "\" --crash-child \"" +
                      dir.string() + "\"";
#ifdef _WIN32
    // cmd.exe strips the first and last quote from the /c argument; wrap
    // the whole command line in one more pair so both paths stay quoted.
    cmd = "\"" + cmd + "\"";
#endif
    const int rc = std::system(cmd.c_str());

    // The child dies via abort — any nonzero exit is fine, but it must not
    // exit cleanly.
    CHECK(rc != 0);

    // State snapshot sidecar for the terminate path.
    CHECK(findArtifact(dir, "-terminate.json"));

    // Exception message .txt with the thrown what().
    fs::path txt;
    CHECK(findArtifact(dir, "-terminate.txt", &txt));
    if (!txt.empty()) {
        std::ifstream f(txt);
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK(ss.str().find("triggerCrashForTesting(Throw)") !=
              std::string::npos);
    }

#ifdef _WIN32
    // The minidump must be written unconditionally — this is the artifact
    // that gets submitted to Sentry on the next launch.
    CHECK(findArtifact(dir, "-terminate.dmp"));
#endif

    if (g_failures != 0) {
        // Show what the child actually left behind so a platform-specific
        // handler mismatch (e.g. "-sigabrt" instead of "-terminate") is
        // visible straight from the CI log.
        std::cerr << "child exit code: " << rc << "\ncrash dir contents:\n";
        for (auto& entry : fs::directory_iterator(dir, ec)) {
            std::cerr << "  " << entry.path().filename().string() << "\n";
        }
    }

    fs::remove_all(dir, ec);

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}
