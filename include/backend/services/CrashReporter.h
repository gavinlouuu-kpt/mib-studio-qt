#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace backend::services {

// Installs process-level crash handlers and (optionally) forwards crashes to
// Sentry. On Windows the primary capture mechanism is MiniDumpWriteDump
// (always available via dbghelp), so even when Sentry is not compiled in
// the application still produces a .dmp + state JSON next to each crash.
//
// Call init() as early as possible in main(), ideally before QApplication is
// constructed. Call shutdown() on clean exit so pending events flush.
class CrashReporter {
public:
    struct Config {
        std::string dsn;                    // Sentry DSN. Empty = local-only.
        std::string release;                // e.g. MIB_STUDIO_QT_VERSION
        std::string environment{"production"};
        std::filesystem::path crashDir;     // where .dmp / .json are written
        std::filesystem::path databaseDir;  // Sentry/Crashpad working dir
        double tracesSampleRate{0.20};      // Sentry performance sample rate.
        bool uploadPendingOnStart{true};
        bool installSignalHandlers{true};
        bool installTerminateHandler{true};
        size_t maxRetainedDumps{50};        // per-class retention bound
                                            // (pending .dmp, delivered
                                            // .dmp.sent/.dmp.rejected, orphan
                                            // .json sidecars)
        // Pending-dump upload (MinidumpUploader): at most this many dumps
        // per launch, oldest first, on a background thread; a dump is
        // renamed .dmp.sent only on an HTTP 2xx, .dmp.rejected on a
        // permanent 4xx, and stays .dmp (retried next launch) otherwise.
        size_t maxUploadsPerStart{10};
        int uploadTimeoutMs{60000};         // per dump
        // sentry-native flush budget at shutdown() for live events (the
        // library default of 2 s left queued envelopes behind on close).
        int shutdownTimeoutMs{5000};
    };

    // Returns true if at least the local minidump path is armed. Returns
    // false only if even dbghelp setup failed. Sentry-init failures are
    // logged but do not cause init() to return false (local-only fallback).
    static bool init(const Config& cfg);
    static void shutdown();
    static bool isInitialized();
    static bool isSentryActive();

    // Diagnostic decorations attached to subsequent crash events.
    static void setTag(std::string_view key, std::string_view value);
    static void setContextJson(std::string_view name, std::string_view json);
    static void breadcrumb(std::string_view category,
                           std::string_view message,
                           std::string_view jsonData = {});

    // Register a callback that returns a JSON snapshot of live program state.
    // Invoked from the crash handler to produce the .json sidecar.
    // The function MUST be safe to call from an asynchronous-signal /
    // unhandled-exception context (no allocations beyond std::string is OK
    // in practice; do not take locks that the crashing thread may hold).
    using StateSnapshotFn = std::function<std::string()>;
    static void registerStateMirror(StateSnapshotFn fn);

    // Manually capture (non-fatal) events. Useful for hooks like main's
    // top-level try/catch.
    static void captureMessage(std::string_view message);
    static void captureException(std::string_view what);
    static void capturePerformanceTransaction(std::string_view name,
                                              std::string_view operation,
                                              double durationMs,
                                              std::string_view jsonData = {});

    // For tests and the debug menu: writes a state snapshot + a fake stack
    // trace to crashDir without aborting.
    static bool writeDiagnosticSnapshot(std::string_view reason);

    // For tests: deliberately crash via the requested mechanism. Should
    // ONLY be wired behind a debug-only entry point.
    enum class FaultKind { NullDeref, Abort, Throw };
    [[noreturn]] static void triggerCrashForTesting(FaultKind kind);
};

} // namespace backend::services
