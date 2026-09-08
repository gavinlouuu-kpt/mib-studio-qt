#include "backend/services/CrashReporter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QtGlobal>
#include <QString>

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <dbghelp.h>
#  include <process.h>
#  pragma comment(lib, "dbghelp.lib")
#else
#  include <unistd.h>
#endif

#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
#  include <sentry.h>
#endif

namespace backend::services {

namespace {
#ifdef _WIN32
// Build identity of the running executable (PDB GUID + age, the symbol-server
// key) so a dump can be matched to its PDB later even after the bench
// rebuilt the tree. Read once at init from the CodeView debug directory.
struct BuildIdentity {
    std::string buildId;   // "<GUID hex><age>", e.g. symbol store folder name
    std::string pdbPath;   // PDB path recorded in the image
    std::string exePath;
};
#endif

// Process-global state for the crash handler. All fields are read from the
// signal / SEH context, so they must use atomics / fixed buffers.
struct CrashGlobals {
    std::atomic<bool> initialized{false};
    std::atomic<bool> handlingCrash{false};
    std::atomic<bool> sentryActive{false};
    CrashReporter::Config config{};
    CrashReporter::StateSnapshotFn stateSnapshot;
    std::mutex stateSnapshotMutex;
#ifdef _WIN32
    void* vectoredHandler{nullptr};  // AddVectoredExceptionHandler cookie
#ifdef _WIN32
    BuildIdentity build;             // PDB GUID+age of the running exe (symbol key)
#endif
#endif
};

CrashGlobals& globals() {
    static CrashGlobals g;
    return g;
}

std::string isoTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", &tm);
    return std::string(buf);
}

std::filesystem::path makeCrashFilenameBase(const std::filesystem::path& dir,
                                            const std::string& reason) {
#ifdef _WIN32
    const unsigned pid = ::GetCurrentProcessId();
#else
    const unsigned pid = static_cast<unsigned>(::getpid());
#endif
    std::ostringstream os;
    os << isoTimestamp() << "-pid" << pid << "-" << reason;
    return dir / os.str();
}

double parseSampleRate(const char* value, double fallback) {
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value) return fallback;
    if (parsed < 0.0) return 0.0;
    if (parsed > 1.0) return 1.0;
    return parsed;
}

uint64_t epochMicrosNow() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Captures the registered state snapshot. try_lock avoids deadlock when the
// crashing thread already holds the snapshot mutex; in that case the snapshot
// is skipped entirely — calling it unguarded would race registerStateMirror's
// assignment of the std::function.
std::string snapshotStateJson() {
    auto& g = globals();
    std::unique_lock<std::mutex> lk(g.stateSnapshotMutex, std::defer_lock);
    if (lk.try_lock() && g.stateSnapshot) {
        return g.stateSnapshot();
    }
    return R"({"note":"state mirror unavailable at crash time"})";
}

#ifdef _WIN32
BuildIdentity readBuildIdentity() {
    BuildIdentity id;
    char exe[MAX_PATH]{};
    if (::GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) return id;
    id.exePath = exe;
    HMODULE module = ::GetModuleHandleA(nullptr);
    ULONG size = 0;
    auto* dbg = static_cast<IMAGE_DEBUG_DIRECTORY*>(
        ::ImageDirectoryEntryToDataEx(module, TRUE, IMAGE_DIRECTORY_ENTRY_DEBUG, &size, nullptr));
    if (!dbg || size < sizeof(IMAGE_DEBUG_DIRECTORY)) return id;
    const size_t count = size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < count; ++i) {
        if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || dbg[i].AddressOfRawData == 0) continue;
        struct CvRsds { DWORD sig; GUID guid; DWORD age; char pdb[1]; };
        const auto* cv = reinterpret_cast<const CvRsds*>(
            reinterpret_cast<const BYTE*>(module) + dbg[i].AddressOfRawData);
        if (cv->sig != 0x53445352 /* 'RSDS' */) continue;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%08lX%04hX%04hX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%lu",
                      cv->guid.Data1, cv->guid.Data2, cv->guid.Data3,
                      cv->guid.Data4[0], cv->guid.Data4[1], cv->guid.Data4[2], cv->guid.Data4[3],
                      cv->guid.Data4[4], cv->guid.Data4[5], cv->guid.Data4[6], cv->guid.Data4[7],
                      static_cast<unsigned long>(cv->age));
        id.buildId = buf;
        id.pdbPath = cv->pdb;
        break;
    }
    return id;
}

// JSON fragment describing a hardware exception: code, address, module and
// offset, faulting thread. Crash-handler safe (fixed buffers, no allocation
// beyond the returned string).
std::string describeException(EXCEPTION_POINTERS* eptr, const BuildIdentity& id) {
    char buf[1024];
    if (!eptr || !eptr->ExceptionRecord) {
        std::snprintf(buf, sizeof(buf), R"({"exception_context":"unavailable","exe_build_id":"%s"})",
                      id.buildId.c_str());
        return buf;
    }
    const EXCEPTION_RECORD* rec = eptr->ExceptionRecord;
    const auto address = reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress);
    char moduleName[MAX_PATH] = "";
    std::uintptr_t offset = 0;
    HMODULE module = nullptr;
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &module) && module) {
        ::GetModuleFileNameA(module, moduleName, MAX_PATH);
        offset = address - reinterpret_cast<std::uintptr_t>(module);
    }
    const char* base = std::strrchr(moduleName, '\\');
    base = base ? base + 1 : moduleName;
    const unsigned long long p0 = rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0;
    const unsigned long long p1 = rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0;
    std::snprintf(buf, sizeof(buf),
                  R"({"code":"0x%08lX","address":"0x%llX","module":"%s","module_offset":"0x%llX","thread_id":%lu,)"
                  R"("access":"%s","target":"0x%llX","exe_build_id":"%s"})",
                  static_cast<unsigned long>(rec->ExceptionCode),
                  static_cast<unsigned long long>(address), base,
                  static_cast<unsigned long long>(offset), ::GetCurrentThreadId(),
                  rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? (p0 == 0 ? "read" : p0 == 1 ? "write" : "execute") : "n/a",
                  p1, id.buildId.c_str());
    return buf;
}
#endif // _WIN32

// Append a "crash" object to the state snapshot JSON (which is an object).
std::string withCrashObject(std::string state, const std::string& crashJson) {
    const auto close = state.find_last_of('}');
    if (close == std::string::npos) return state;
    state.insert(close, std::string(",\"crash\":") + crashJson);
    return state;
}

// Safe to call from a crash handler — only uses C-runtime file APIs.
void writeStateJsonSidecar(const std::filesystem::path& jsonPath,
                           const std::string& json) {
    std::FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, jsonPath.string().c_str(), "wb") != 0) f = nullptr;
#else
    f = std::fopen(jsonPath.string().c_str(), "wb");
#endif
    if (!f) return;
    std::fwrite(json.data(), 1, json.size(), f);
    std::fflush(f);
    std::fclose(f);
}

void writeStateJsonSidecar(const std::filesystem::path& jsonPath) {
    writeStateJsonSidecar(jsonPath, snapshotStateJson());
}

bool endsWith(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
sentry_value_t onCrashHook(const sentry_ucontext_t* uctx,
                           sentry_value_t event,
                           void* closure) {
    (void)uctx;
    (void)closure;
    auto& g = globals();

    // Another thread is already inside a crash handler: do no further work
    // (heap/lock state is unknown); let Crashpad finish the minidump.
    bool expected = false;
    if (!g.handlingCrash.compare_exchange_strong(expected, true)) {
        return event;
    }

    try {
        // Snapshot once; reuse for both the local sidecar and the event.
        const std::string json = snapshotStateJson();

        std::error_code ec;
        std::filesystem::create_directories(g.config.crashDir, ec);
        const auto base = makeCrashFilenameBase(g.config.crashDir, "crashpad");
        writeStateJsonSidecar(std::filesystem::path(base.string() + ".json"),
                              json);

        // Mutate the event itself rather than the scope: with the crashpad
        // backend, scope changes made this late may not reach the uploaded
        // event, but the returned event does.
        sentry_value_t extra = sentry_value_get_by_key(event, "extra");
        if (sentry_value_is_null(extra)) {
            extra = sentry_value_new_object();
            sentry_value_set_by_key(event, "extra", extra);
        }
        sentry_value_set_by_key(extra, "state_snapshot",
                                sentry_value_new_string(json.c_str()));
    } catch (...) {}

    return event;
}
#endif

#ifdef _WIN32
LONG writeMinidumpInternal(EXCEPTION_POINTERS* eptr,
                           const std::filesystem::path& dumpPath) {
    HANDLE hFile = ::CreateFileA(dumpPath.string().c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MINIDUMP_EXCEPTION_INFORMATION mdei{};
    if (eptr) {
        mdei.ThreadId = ::GetCurrentThreadId();
        mdei.ExceptionPointers = eptr;
        mdei.ClientPointers = FALSE;
    }

    const MINIDUMP_TYPE mdt = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpScanMemory |
        MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules);

    BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(),
                                   ::GetCurrentProcessId(),
                                   hFile,
                                   mdt,
                                   eptr ? &mdei : nullptr,
                                   nullptr,
                                   nullptr);
    ::CloseHandle(hFile);
    return ok ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

// SEH exception code MSVC raises for a C++ `throw` (ehdata.h's
// EH_EXCEPTION_NUMBER; only the stable numeric code is needed here).
constexpr DWORD kCxxExceptionCode = 0xE06D7363;

// Per-thread record of the most recent C++ throw, filled in by
// cxxThrowVectoredHandler (below). Read by terminateHandler, because on MSVC
// std::current_exception() is null inside a terminate handler for an
// *uncaught* exception (the CRT only populates it while a catch block is
// executing), so this is the only way to get the escaping exception's
// what() into the "-terminate.txt" note.
struct LastThrowRecord {
    bool valid{false};
    char what[512]{};
};
thread_local LastThrowRecord t_lastThrow;
thread_local bool t_terminateHandlerInstalled = false;

void terminateHandler();

// Layout of the MSVC C++ EH metadata reachable from an EXCEPTION_RECORD with
// code kCxxExceptionCode (ehdata.h). On x64/ARM64 every "pointer" below is a
// 32-bit image-relative offset from ExceptionInformation[3]. Only the fields
// needed to locate a std::exception subobject are modelled.
struct MsvcPmd { int mdisp; int pdisp; int vdisp; };
struct MsvcCatchableType {
    unsigned properties;
    int pType;                 // -> MsvcTypeDescriptor
    MsvcPmd thisDisplacement;  // adjustment from the thrown object to this base
    int sizeOrOffset;
    int copyFunction;
};
struct MsvcCatchableTypeArray { int nCatchableTypes; int arrayOfCatchableTypes[1]; };
struct MsvcThrowInfo { unsigned attributes; int pmfnUnwind; int pForwardCompat; int pCatchableTypeArray; };
struct MsvcTypeDescriptor { const void* pVFTable; void* spare; char name[1]; };

// Copies what() of the thrown object into `out` when the object is catchable
// as std::exception. Runs at throw time on the throwing thread. SEH-guarded
// and free of C++ objects with destructors (a __try function may not have
// any) so a malformed record can never turn a plain throw into a crash.
bool extractStdExceptionWhat(const EXCEPTION_RECORD* rec, char* out, size_t outSize) noexcept {
#if defined(_M_X64) || defined(_M_ARM64)
    __try {
        if (rec->NumberParameters < 4) return false;
        const ULONG_PTR magic = rec->ExceptionInformation[0];
        if (magic < 0x19930520u || magic > 0x19930522u) return false;  // EH_MAGIC_NUMBER1..3
        char* object = reinterpret_cast<char*>(rec->ExceptionInformation[1]);
        const auto* throwInfo = reinterpret_cast<const MsvcThrowInfo*>(rec->ExceptionInformation[2]);
        const char* base = reinterpret_cast<const char*>(rec->ExceptionInformation[3]);
        if (!object || !throwInfo || !base || throwInfo->pCatchableTypeArray == 0) return false;

        const auto* cta = reinterpret_cast<const MsvcCatchableTypeArray*>(base + throwInfo->pCatchableTypeArray);
        const int n = cta->nCatchableTypes;
        if (n <= 0 || n > 64) return false;
        for (int i = 0; i < n; ++i) {
            const auto* ct = reinterpret_cast<const MsvcCatchableType*>(base + cta->arrayOfCatchableTypes[i]);
            const auto* td = reinterpret_cast<const MsvcTypeDescriptor*>(base + ct->pType);
            if (std::strcmp(td->name, ".?AVexception@std@@") != 0) continue;

            char* p = object;
            if (ct->thisDisplacement.pdisp >= 0) {
                const char* vbtable = *reinterpret_cast<char* const*>(p + ct->thisDisplacement.pdisp);
                p += ct->thisDisplacement.pdisp +
                     *reinterpret_cast<const int*>(vbtable + ct->thisDisplacement.vdisp);
            }
            p += ct->thisDisplacement.mdisp;
            const char* what = reinterpret_cast<const std::exception*>(p)->what();
            if (!what) return false;
            strncpy_s(out, outSize, what, _TRUNCATE);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    (void)rec; (void)out; (void)outSize;
#endif
    return false;
}

// First-chance vectored handler: runs on the throwing thread for every C++
// throw in the process, before any frame handler. Two jobs, both needed to
// make "-terminate" artifacts work on Windows:
//
//  1. MSVC's set_terminate is PER-THREAD and not inherited (MS docs: "each
//     new thread needs to install its own terminate function"). init() can
//     only install terminateHandler on the calling thread, so an exception
//     escaping a worker thread hit the CRT default (plain abort()) and only
//     ever produced "-sigabrt" artifacts via the SIGABRT fallback. Installing
//     the handler here, lazily on first throw, covers every thread that can
//     possibly reach terminate() through an exception.
//  2. Capture what() now (see LastThrowRecord).
//
// Always returns EXCEPTION_CONTINUE_SEARCH: it never handles anything, so it
// cannot interfere with normal catch dispatch or with Crashpad.
LONG WINAPI cxxThrowVectoredHandler(EXCEPTION_POINTERS* eptr) {
    if (!eptr || !eptr->ExceptionRecord ||
        eptr->ExceptionRecord->ExceptionCode != kCxxExceptionCode) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!t_terminateHandlerInstalled) {
        std::set_terminate(terminateHandler);
        t_terminateHandlerInstalled = true;
    }
    // A rethrow (`throw;`) carries no object pointer; keep the original.
    const EXCEPTION_RECORD* rec = eptr->ExceptionRecord;
    if (rec->NumberParameters >= 2 && rec->ExceptionInformation[1] != 0) {
        t_lastThrow.valid = extractStdExceptionWhat(rec, t_lastThrow.what, sizeof(t_lastThrow.what));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI sehHandler(EXCEPTION_POINTERS* eptr) {
    // A C++ exception only reaches the top-level filter when no frame claimed
    // it AND no noexcept frame terminated it first (e.g. off a thread whose
    // entry point is not noexcept). Installing this filter displaced the
    // CRT's own __CxxUnhandledExceptionFilter, whose sole job is to turn that
    // into std::terminate(); do the same so the terminate path (handler
    // installed per-thread by cxxThrowVectoredHandler) owns the artifacts.
    if (eptr && eptr->ExceptionRecord &&
        eptr->ExceptionRecord->ExceptionCode == kCxxExceptionCode) {
        std::terminate();
    }

    auto& g = globals();
    bool expected = false;
    if (!g.handlingCrash.compare_exchange_strong(expected, true)) {
        // Already handling a crash; avoid recursion.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    try {
        std::error_code ec;
        std::filesystem::create_directories(g.config.crashDir, ec);
        const auto base = makeCrashFilenameBase(g.config.crashDir, "seh");
        const auto dump = std::filesystem::path(base.string() + ".dmp");
        const auto json = std::filesystem::path(base.string() + ".json");
        writeMinidumpInternal(eptr, dump);
        writeStateJsonSidecar(json, withCrashObject(snapshotStateJson(), describeException(eptr, g.build)));
    } catch (...) {
        // Swallow — we are already crashing.
    }

#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    // sentry-native's own SEH handler is installed by sentry_init; let it run.
    return EXCEPTION_CONTINUE_SEARCH;
#else
    return EXCEPTION_CONTINUE_SEARCH; // let WER / debugger pick it up too
#endif
}
#endif // _WIN32

extern "C" void signalHandler(int sig) {
    auto& g = globals();
    bool expected = false;
    if (!g.handlingCrash.compare_exchange_strong(expected, true)) {
        std::_Exit(128 + sig);
    }
    const char* name = "signal";
    switch (sig) {
        case SIGSEGV: name = "sigsegv"; break;
        case SIGABRT: name = "sigabrt"; break;
        case SIGFPE:  name = "sigfpe"; break;
        case SIGILL:  name = "sigill"; break;
        default: break;
    }
    try {
        std::error_code ec;
        std::filesystem::create_directories(g.config.crashDir, ec);
        const auto base = makeCrashFilenameBase(g.config.crashDir, name);
#ifdef _WIN32
        // The MSVC CRT invokes SIGSEGV/SIGFPE/SIGILL handlers from its own
        // SEH filter and publishes the EXCEPTION_POINTERS in _pxcptinfoptrs.
        // Without them the dump's exception record was the dump writer's own
        // breakpoint and !analyze showed nothing (55 useless dumps on the
        // bench, crash review 2026-09-08).
        EXCEPTION_POINTERS* eptr = nullptr;
        if (sig == SIGSEGV || sig == SIGFPE || sig == SIGILL) {
            eptr = static_cast<EXCEPTION_POINTERS*>(_pxcptinfoptrs);
        }
        const auto dump = std::filesystem::path(base.string() + ".dmp");
        writeMinidumpInternal(eptr, dump);
        const auto json = std::filesystem::path(base.string() + ".json");
        writeStateJsonSidecar(json, withCrashObject(snapshotStateJson(), describeException(eptr, g.build)));
#else
        const auto json = std::filesystem::path(base.string() + ".json");
        writeStateJsonSidecar(json);
#endif
    } catch (...) {
        // Crashing already — never throw out of here.
    }
    // Re-raise with default disposition so debuggers / WER still get it.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void terminateHandler() {
    auto& g = globals();
    bool expected = false;
    if (!g.handlingCrash.compare_exchange_strong(expected, true)) {
        std::abort();
    }
    try {
        std::error_code ec;
        std::filesystem::create_directories(g.config.crashDir, ec);
        const auto base = makeCrashFilenameBase(g.config.crashDir, "terminate");
        const auto json = std::filesystem::path(base.string() + ".json");
        writeStateJsonSidecar(json);

        // When terminate() was reached via an unhandled exception, record
        // its message alongside the snapshot (same .txt convention as
        // captureException).
        std::string what;
        bool haveWhat = false;
        if (std::current_exception()) {
            what = "unknown (non-std::exception)";
            haveWhat = true;
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
        }
#ifdef _WIN32
        // MSVC: current_exception() is null for an uncaught exception; use
        // the record cxxThrowVectoredHandler captured at throw time on this
        // thread (best effort — it is the last throw on this thread).
        else if (t_lastThrow.valid) {
            what = t_lastThrow.what;
            haveWhat = true;
        }
#endif
        if (haveWhat) {
            std::ofstream f(base.string() + ".txt");
            f << "terminate: " << what << "\n";
        }

#ifdef _WIN32
        // Write the minidump unconditionally, even with Sentry active. The
        // abort() below is intercepted by our SIGABRT handler, which
        // _Exit()s while handlingCrash is already set — so neither Crashpad
        // nor the abort fallback ever produces a dump for this path. This
        // dump rides the pending-upload path on the next launch.
        const auto dump = std::filesystem::path(base.string() + ".dmp");
        writeMinidumpInternal(nullptr, dump);
#endif
    } catch (...) {
    }
    std::abort();
}

void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    const std::string m = msg.toStdString();
    const char* file = ctx.file ? ctx.file : "";
    const char* fn   = ctx.function ? ctx.function : "";

    switch (type) {
        case QtDebugMsg:    SPDLOG_DEBUG("[Qt] {} ({}:{} {})", m, file, ctx.line, fn); break;
        case QtInfoMsg:     SPDLOG_INFO ("[Qt] {} ({}:{} {})", m, file, ctx.line, fn); break;
        case QtWarningMsg:  SPDLOG_WARN ("[Qt] {} ({}:{} {})", m, file, ctx.line, fn); break;
        case QtCriticalMsg: SPDLOG_ERROR("[Qt] {} ({}:{} {})", m, file, ctx.line, fn);
                             CrashReporter::captureMessage("Qt critical: " + m); break;
        case QtFatalMsg:    SPDLOG_CRITICAL("[Qt FATAL] {} ({}:{} {})", m, file, ctx.line, fn);
                             CrashReporter::captureMessage("Qt fatal: " + m); break;
    }
}

void recoverLegacyUploaded(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    // Collect first, rename after: renaming while a directory_iterator is
    // live can skip entries (FindNextFile semantics on Windows).
    std::vector<std::filesystem::path> legacy;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (endsWith(entry.path().string(), ".dmp.uploaded")) {
            legacy.push_back(entry.path());
        }
    }

    for (const auto& p : legacy) {
        const std::string pathStr = p.string();
        const std::string dmpSuffix = ".dmp.uploaded";
        const std::string base = pathStr.substr(0, pathStr.size() - dmpSuffix.size());
        const std::string restoredDmp = base + ".dmp";
        std::filesystem::rename(p, restoredDmp, ec);
        if (ec) {
            SPDLOG_WARN("CrashReporter: failed to recover legacy dump {}: {}",
                        pathStr, ec.message());
            ec.clear();
            continue;
        }
        SPDLOG_INFO("CrashReporter: recovered legacy dump for re-upload: {}",
                    restoredDmp);

        const std::string sidecarUploaded = base + ".json.uploaded";
        if (std::filesystem::exists(sidecarUploaded, ec)) {
            std::filesystem::rename(sidecarUploaded, base + ".json", ec);
            ec.clear();
        }
    }
}

struct RetainedEntry {
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
};

// Removes the oldest entries beyond maxCount. removeSibling maps an entry to
// an optional companion file removed alongside it (best-effort).
void trimOldest(std::vector<RetainedEntry>& entries, size_t maxCount,
                const char* what,
                const std::function<std::string(const std::string&)>& sibling) {
    if (entries.size() <= maxCount) return;

    std::sort(entries.begin(), entries.end(),
              [](const RetainedEntry& a, const RetainedEntry& b) {
                  return a.modified < b.modified;
              });

    std::error_code ec;
    const size_t toRemove = entries.size() - maxCount;
    for (size_t i = 0; i < toRemove; ++i) {
        std::filesystem::remove(entries[i].path, ec);
        if (ec) {
            SPDLOG_WARN("CrashReporter: failed to remove old {} {}: {}",
                        what, entries[i].path.string(), ec.message());
            ec.clear();
            continue;
        }
        SPDLOG_INFO("CrashReporter: removed old {} (retention limit {}): {}",
                    what, maxCount, entries[i].path.string());

        if (sibling) {
            const std::string companion = sibling(entries[i].path.string());
            if (!companion.empty()) {
                std::filesystem::remove(companion, ec);
                ec.clear();
            }
        }
    }
}

void cleanupRetainedDumps(const std::filesystem::path& dir, size_t maxCount) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    std::vector<RetainedEntry> queued;
    // Pending .dmp files that never got submitted (local-only installs, or
    // Sentry inactive for many launches): bound them too so a no-DSN
    // install cannot grow the crash dir without limit. This runs AFTER
    // uploadPendingCrashes, so with Sentry active this set is empty.
    std::vector<RetainedEntry> pendingDumps;
    // Bare .json sidecars with no dump (terminate / on_crash / diagnostic
    // paths): nothing uploads or renames them, so bound them here too.
    std::vector<RetainedEntry> orphanJson;
    // Bare .txt notes whose .json and .dmp companions are both gone
    // (renamed away by upload, or already trimmed): bound them as well.
    std::vector<RetainedEntry> orphanTxt;

    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string pathStr = entry.path().string();
        if (endsWith(pathStr, ".dmp.queued") ||
            endsWith(pathStr, ".dmp.queued2")) {
            queued.push_back({entry.path(), entry.last_write_time(ec)});
            ec.clear();
        } else if (endsWith(pathStr, ".dmp")) {
            pendingDumps.push_back({entry.path(), entry.last_write_time(ec)});
            ec.clear();
        } else if (endsWith(pathStr, ".txt")) {
            const std::string base = pathStr.substr(0, pathStr.size() - 4);
            const bool hasCompanion =
                std::filesystem::exists(base + ".json", ec) ||
                std::filesystem::exists(base + ".dmp", ec);
            ec.clear();
            if (!hasCompanion) {
                orphanTxt.push_back({entry.path(), entry.last_write_time(ec)});
                ec.clear();
            }
        } else if (endsWith(pathStr, ".json")) {
            // Keep the sidecar of any dump still on disk (pending or queued).
            const std::string base = pathStr.substr(0, pathStr.size() - 5);
            const bool hasDump =
                std::filesystem::exists(base + ".dmp", ec) ||
                std::filesystem::exists(base + ".dmp.queued", ec);
            ec.clear();
            if (!hasDump) {
                orphanJson.push_back({entry.path(), entry.last_write_time(ec)});
                ec.clear();
            }
        }
    }

    trimOldest(queued, maxCount, "queued dump",
               [](const std::string& p) {
                   if (endsWith(p, ".dmp.queued2")) {
                       const std::string suffix = ".dmp.queued2";
                       return p.substr(0, p.size() - suffix.size()) +
                              ".json.queued2";
                   }
                   const std::string suffix = ".dmp.queued";
                   return p.substr(0, p.size() - suffix.size()) + ".json.queued";
               });
    trimOldest(pendingDumps, maxCount, "pending dump",
               [](const std::string& p) {
                   return p.substr(0, p.size() - 4) + ".json";
               });
    trimOldest(orphanJson, maxCount, "orphan sidecar",
               [](const std::string& p) {
                   // captureException writes a .txt next to its .json.
                   return p.substr(0, p.size() - 5) + ".txt";
               });
    trimOldest(orphanTxt, maxCount, "orphan text note", {});
}

void uploadPendingCrashes(const std::filesystem::path& dir) {
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    // Without a live Sentry (no DSN, or sentry_init failed) capture calls are
    // no-ops: leave the dumps untouched so a later launch with Sentry active
    // can still submit them. Renaming here would mark never-sent dumps as
    // queued and retention would eventually destroy them.
    if (!globals().sentryActive.load()) {
        SPDLOG_INFO("CrashReporter: Sentry inactive; leaving pending dumps in place");
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    const int retryAfterDays = globals().config.queuedRetryAfterDays;

    // Collect first, act after: renaming while a directory_iterator is live
    // can skip entries (FindNextFile semantics on Windows).
    struct Submission {
        std::filesystem::path dmp;
        std::filesystem::path sidecar;
        std::filesystem::path message;  // optional <base>.txt (terminate path)
        bool isRetry;
    };
    std::vector<Submission> pending;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string pathStr = entry.path().string();
        if (endsWith(pathStr, ".dmp")) {
            const std::string base = pathStr.substr(0, pathStr.size() - 4);
            pending.push_back(
                {entry.path(), base + ".json", base + ".txt", false});
        } else if (retryAfterDays > 0 && endsWith(pathStr, ".dmp.queued")) {
            // A .queued dump whose envelope was dropped (send failed, or the
            // process died before the shutdown queue dump) gets exactly one
            // re-submission once it has sat queued long enough that the
            // transport clearly never delivered it (its mtime is bumped at
            // submission time below). .queued2 is terminal.
            const auto age = std::filesystem::file_time_type::clock::now() -
                             std::filesystem::last_write_time(entry.path(), ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (age > std::chrono::hours(24) * retryAfterDays) {
                const std::string base =
                    pathStr.substr(0, pathStr.size() - sizeof(".dmp.queued") + 1);
                pending.push_back(
                    {entry.path(), base + ".json.queued", base + ".txt", true});
            }
        }
    }

    for (const auto& sub : pending) {
        const auto& p = sub.dmp;
        bool hasSidecar = std::filesystem::exists(sub.sidecar, ec);

        if (hasSidecar) {
            std::ifstream f(sub.sidecar);
            std::stringstream ss;
            ss << f.rdbuf();
            sentry_set_extra("state_snapshot",
                             sentry_value_new_string(ss.str().c_str()));
        }

        // Terminate-path dumps carry the unhandled exception's what() in a
        // .txt note — attach it so the message reaches the event.
        bool hasMessage = std::filesystem::exists(sub.message, ec);
        if (hasMessage) {
            std::ifstream f(sub.message);
            std::stringstream ss;
            ss << f.rdbuf();
            sentry_set_extra("crash_message",
                             sentry_value_new_string(ss.str().c_str()));
        }

        sentry_set_tag("crash_recovery",
                       sub.isRetry ? "queued_retry" : "pending_dump");
        sentry_set_extra("original_dump_file",
                         sentry_value_new_string(p.filename().string().c_str()));

        // sentry_capture_minidump reads the dump into an envelope and
        // generates a fatal event with an event.minidump attachment. In
        // 0.7.20 it returns void, so per-capture success cannot be checked.
        // Durability (verified against the pinned sentry-native 0.7.20
        // source): envelopes still WAITING in the transport queue at
        // shutdown are dumped into the database .run folder and re-sent on
        // a later launch (sentry__transport_dump_queue +
        // sentry__process_old_runs); an envelope whose send attempt FAILS
        // (e.g. offline) is freed without retry. The rename below is
        // therefore optimistic — the stale-.queued retry pass above covers
        // the send-failure loss window.
        sentry_capture_minidump(p.string().c_str());

        sentry_remove_extra("state_snapshot");
        sentry_remove_extra("crash_message");
        sentry_remove_extra("original_dump_file");
        sentry_remove_tag("crash_recovery");

        SPDLOG_INFO("CrashReporter: submitted {} minidump: {}",
                    sub.isRetry ? "stale queued" : "pending", p.string());

        // Pending: .dmp → .dmp.queued. Retry: .dmp.queued → .dmp.queued2
        // (terminal — never picked up again).
        const std::filesystem::path renamed(
            p.string() + (sub.isRetry ? "2" : ".queued"));
        std::filesystem::rename(p, renamed, ec);
        if (ec) {
            SPDLOG_WARN("CrashReporter: failed to rename {}: {}",
                        p.string(), ec.message());
            ec.clear();
        } else if (!sub.isRetry) {
            // rename() preserves mtime, which for a fresh .dmp is the CRASH
            // time — a dump submitted more than retryAfterDays after the
            // crash would look stale immediately and be duplicated on the
            // next launch. Stamp the submission time instead so the retry
            // clock starts now.
            std::filesystem::last_write_time(
                renamed, std::filesystem::file_time_type::clock::now(), ec);
            ec.clear();
        }
        if (hasSidecar && std::filesystem::exists(sub.sidecar, ec)) {
            std::filesystem::rename(
                sub.sidecar,
                sub.sidecar.string() + (sub.isRetry ? "2" : ".queued"), ec);
            ec.clear();
        }
    }
#else
    (void)dir;
#endif
}

} // namespace

bool CrashReporter::init(const Config& cfg) {
    auto& g = globals();
    if (g.initialized.load()) return true;

    g.config = cfg;
    std::error_code ec;
    if (!g.config.crashDir.empty()) {
        std::filesystem::create_directories(g.config.crashDir, ec);
    }
    if (!g.config.databaseDir.empty()) {
        std::filesystem::create_directories(g.config.databaseDir, ec);
    }

#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    if (!cfg.dsn.empty()) {
        sentry_options_t* options = sentry_options_new();
        sentry_options_set_dsn(options, cfg.dsn.c_str());
        if (!cfg.release.empty()) {
            sentry_options_set_release(options, cfg.release.c_str());
        }
        sentry_options_set_environment(options, cfg.environment.c_str());
        const double tracesSampleRate =
            parseSampleRate(std::getenv("MIB_SENTRY_TRACES_SAMPLE_RATE"),
                            cfg.tracesSampleRate);
        sentry_options_set_traces_sample_rate(options, tracesSampleRate);
        if (!cfg.databaseDir.empty()) {
            sentry_options_set_database_path(options, cfg.databaseDir.string().c_str());
        }
        sentry_options_set_auto_session_tracking(options, 1);
        sentry_options_set_symbolize_stacktraces(options, 1);

        sentry_options_set_on_crash(options, onCrashHook, nullptr);

        if (sentry_init(options) != 0) {
            SPDLOG_WARN("CrashReporter: sentry_init failed; continuing local-only");
        } else {
            g.sentryActive.store(true);
            SPDLOG_INFO("CrashReporter: Sentry initialized (release={}, env={}, tracesSampleRate={})",
                        cfg.release, cfg.environment, tracesSampleRate);
        }
    } else {
        SPDLOG_INFO("CrashReporter: no DSN configured; running in local-only mode");
    }
#else
    SPDLOG_INFO("CrashReporter: built without Sentry support; running in local-only mode");
#endif

    // Install local fault handlers only when Sentry/Crashpad is NOT active.
    // When Sentry owns the crash backend, competing handlers would intercept
    // faults before Crashpad and prevent it from generating real minidump
    // events. The on_crash callback writes local JSON sidecars instead.
    if (!g.sentryActive.load()) {
#ifdef _WIN32
        g.build = readBuildIdentity();
        SPDLOG_INFO("CrashReporter: exe {} build id {} pdb {}", g.build.exePath,
                    g.build.buildId.empty() ? "unknown" : g.build.buildId,
                    g.build.pdbPath.empty() ? "none" : g.build.pdbPath);
        // Dev builds run straight from the build tree, which the next build
        // overwrites; keep this binary's PDB under <crashDir>/../symbols/<id>
        // so its dumps stay symbolizable (cdbX64 -y that folder).
        if (!g.build.buildId.empty()) {
            std::error_code ec;
            const std::filesystem::path pdbNext =
                std::filesystem::path(g.build.exePath).replace_extension(".pdb");
            const std::filesystem::path store =
                g.config.crashDir.parent_path() / "symbols" / g.build.buildId;
            const std::filesystem::path kept = store / pdbNext.filename();
            if (std::filesystem::is_regular_file(pdbNext, ec) && !std::filesystem::exists(kept, ec)) {
                std::filesystem::create_directories(store, ec);
                std::filesystem::copy_file(pdbNext, kept, ec);
                if (!ec) SPDLOG_INFO("CrashReporter: kept PDB for build {} at {}", g.build.buildId, kept.string());
            }
        }
        ::SetUnhandledExceptionFilter(sehHandler);
#endif
        if (cfg.installSignalHandlers) {
            std::signal(SIGSEGV, signalHandler);
            std::signal(SIGABRT, signalHandler);
            std::signal(SIGFPE,  signalHandler);
            std::signal(SIGILL,  signalHandler);
        }
    } else {
        // Crashpad only intercepts native faults (SEH); a CRT abort() never
        // reaches it. Keep a SIGABRT handler as the local fallback so aborts
        // still produce a dump + sidecar, then re-raise with SIG_DFL.
        // std::terminate is likewise invisible to Crashpad — terminateHandler
        // writes its own dump + sidecar before aborting (see above).
        if (cfg.installSignalHandlers) {
            std::signal(SIGABRT, signalHandler);
        }
        SPDLOG_INFO("CrashReporter: Sentry active — Crashpad owns fault capture; "
                    "local SIGABRT fallback installed");
    }

    if (cfg.installTerminateHandler) {
        std::set_terminate(terminateHandler);
#ifdef _WIN32
        // set_terminate above only covers the calling thread on MSVC; the
        // vectored handler installs it on every other thread at its first
        // throw and records the thrown what() (see cxxThrowVectoredHandler).
        // First-chance (1) so it runs before any frame handler, including
        // the noexcept std::thread entry shim that terminates escaping
        // exceptions during the search phase.
        if (!g.vectoredHandler) {
            g.vectoredHandler = ::AddVectoredExceptionHandler(1, cxxThrowVectoredHandler);
        }
        t_terminateHandlerInstalled = true;
#endif
    }

    if (cfg.installQtMessageHandler) {
        qInstallMessageHandler(qtMessageHandler);
    }

    g.initialized.store(true);
    SPDLOG_INFO("CrashReporter initialized: crashDir={}", cfg.crashDir.string());

    if (cfg.uploadPendingOnStart) {
        recoverLegacyUploaded(cfg.crashDir);
        uploadPendingCrashes(cfg.crashDir);
        cleanupRetainedDumps(cfg.crashDir, cfg.maxRetainedDumps);
    }
    return true;
}

void CrashReporter::shutdown() {
    auto& g = globals();
    if (!g.initialized.load()) return;
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    sentry_close();
#endif
#ifdef _WIN32
    if (g.vectoredHandler) {
        ::RemoveVectoredExceptionHandler(g.vectoredHandler);
        g.vectoredHandler = nullptr;
    }
#endif
    g.sentryActive.store(false);
    g.initialized.store(false);
}

bool CrashReporter::isInitialized() {
    return globals().initialized.load();
}

bool CrashReporter::isSentryActive() {
    return globals().sentryActive.load();
}

void CrashReporter::setTag(std::string_view key, std::string_view value) {
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    if (!globals().initialized.load()) return;
    sentry_set_tag(std::string(key).c_str(), std::string(value).c_str());
#else
    (void)key; (void)value;
#endif
}

void CrashReporter::setContextJson(std::string_view name, std::string_view json) {
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    if (!globals().initialized.load()) return;
    sentry_value_t v = sentry_value_new_string(std::string(json).c_str());
    sentry_set_extra(std::string(name).c_str(), v);
#else
    (void)name; (void)json;
#endif
}

void CrashReporter::breadcrumb(std::string_view category,
                                std::string_view message,
                                std::string_view jsonData) {
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    if (!globals().initialized.load()) return;
    sentry_value_t crumb = sentry_value_new_breadcrumb(
        std::string(category).c_str(),
        std::string(message).c_str());
    if (!jsonData.empty()) {
        sentry_value_set_by_key(crumb, "data",
            sentry_value_new_string(std::string(jsonData).c_str()));
    }
    sentry_add_breadcrumb(crumb);
#else
    (void)category; (void)message; (void)jsonData;
#endif
}

void CrashReporter::registerStateMirror(StateSnapshotFn fn) {
    auto& g = globals();
    std::scoped_lock lk(g.stateSnapshotMutex);
    g.stateSnapshot = std::move(fn);
}

void CrashReporter::captureMessage(std::string_view message) {
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    if (!globals().initialized.load()) return;
    sentry_value_t ev = sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "app",
                                                       std::string(message).c_str());
    sentry_capture_event(ev);
#else
    (void)message;
#endif
}

void CrashReporter::captureException(std::string_view what) {
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    if (!globals().initialized.load()) return;
    sentry_value_t ev = sentry_value_new_message_event(SENTRY_LEVEL_FATAL, "exception",
                                                       std::string(what).c_str());
    sentry_capture_event(ev);
#endif
    // Always write a local sidecar so we have something on disk even
    // without Sentry.
    try {
        const auto base = makeCrashFilenameBase(globals().config.crashDir, "exception");
        writeStateJsonSidecar(std::filesystem::path(base.string() + ".json"));

        // Also write a small text file with the exception message.
        std::ofstream f(base.string() + ".txt");
        f << "exception: " << what << "\n";
    } catch (...) {
    }
}

void CrashReporter::capturePerformanceTransaction(std::string_view name,
                                                  std::string_view operation,
                                                  double durationMs,
                                                  std::string_view jsonData) {
#if defined(MIB_USE_SENTRY) && MIB_USE_SENTRY
    if (!globals().initialized.load()) return;
    if (name.empty() || operation.empty() || durationMs < 0.0) return;

    const std::string nameStr(name);
    const std::string opStr(operation);
    sentry_transaction_context_t* ctx =
        sentry_transaction_context_new(nameStr.c_str(), opStr.c_str());
    if (!ctx) return;

    const uint64_t finishUs = epochMicrosNow();
    const auto durationUs = static_cast<uint64_t>(durationMs * 1000.0);
    const uint64_t startUs = finishUs > durationUs ? finishUs - durationUs : finishUs;
    sentry_transaction_t* tx =
        sentry_transaction_start_ts(ctx, sentry_value_new_null(), startUs);
    if (!tx) return;

    sentry_transaction_set_tag(tx, "component", "mib-studio-qt");
    sentry_transaction_set_data(tx, "duration_ms", sentry_value_new_double(durationMs));
    if (!jsonData.empty()) {
        sentry_transaction_set_data(tx, "perf_data",
                                    sentry_value_new_string(std::string(jsonData).c_str()));
    }
    sentry_transaction_finish_ts(tx, finishUs);
#else
    (void)name;
    (void)operation;
    (void)durationMs;
    (void)jsonData;
#endif
}

bool CrashReporter::writeDiagnosticSnapshot(std::string_view reason) {
    if (!globals().initialized.load()) return false;
    try {
        const std::string r(reason.empty() ? std::string_view{"manual"} : reason);
        const auto base = makeCrashFilenameBase(globals().config.crashDir, r);
        writeStateJsonSidecar(std::filesystem::path(base.string() + ".json"));
        SPDLOG_INFO("CrashReporter: wrote diagnostic snapshot: {}", base.string());
        return true;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("CrashReporter: snapshot failed: {}", e.what());
        return false;
    }
}

[[noreturn]] void CrashReporter::triggerCrashForTesting(FaultKind kind) {
    SPDLOG_CRITICAL("CrashReporter: triggerCrashForTesting kind={}", static_cast<int>(kind));
    switch (kind) {
        case FaultKind::NullDeref: {
            volatile int* p = nullptr;
            *p = 0;
            break;
        }
        case FaultKind::Abort:
            std::abort();
        case FaultKind::Throw:
            throw std::runtime_error("triggerCrashForTesting(Throw)");
    }
    std::abort();
}

} // namespace backend::services
