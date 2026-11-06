// data_directory_test (rig check v1-02, finding F1)
//
// An installed MIB Studio Qt kept its state in C:\Program Files\MIB Studio Qt\data,
// which a normal user cannot write: the instrument identity, calibration
// records, SQLite database and profile cache were never saved. The data dir is
// now resolved once (backend/app/DataDirectory.h):
//  - resolver: every branch on injected inputs (Program Files, unwritable exe
//    dir, writable exe dir, unknown %LOCALAPPDATA%, MIB_DATA_DIR, not Windows),
//    so the Windows rule is proven on any OS; plus the real write probe and the
//    environment read;
//  - migration (round trip + fault injection): state in the legacy
//    <exe dir>/data is copied once into an empty new data dir, byte for byte;
//    logs and mock frames are not; the legacy files are never removed; an
//    unreadable entry fails alone and leaves no partial file; a new dir that
//    already holds state is never touched;
//  - AppBackend: initialize() migrates before it loads the identity, so the
//    migrated id is the one the instrument keeps.
#include "backend/app/AppBackend.h"
#include "backend/app/DataDirectory.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace datadir = backend::app::datadir;

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

// chmod-based faults need POSIX permissions and a non-root user.
bool permissionFaultsWork() {
#ifdef _WIN32
    return false;
#else
    return geteuid() != 0;
#endif
}

struct ProbeLog {
    std::vector<fs::path> calls;
    bool answer = true;
    std::function<bool(const fs::path&)> fn() {
        return [this](const fs::path& p) {
            calls.push_back(p);
            return answer;
        };
    }
};

const fs::path kLocalAppData = fs::path("C:/Users/kenny/AppData/Local");
const fs::path kUserRoot = kLocalAppData / "MIB_Studio_Qt";

datadir::Inputs windowsInputs(const fs::path& exeDir, ProbeLog& probe) {
    datadir::Inputs in;
    in.exeDir = exeDir;
    in.localAppData = kLocalAppData;
    in.windows = true;
    in.writable = probe.fn();
    return in;
}

void resolverRules() {
    // An install under Program Files: the user-local dir, by path alone (no probe,
    // so an elevated launch resolves the same folder as a normal one).
    {
        ProbeLog probe;
        const fs::path exe = "C:/Program Files/MIB Studio Qt";
        const auto layout = datadir::resolve(windowsInputs(exe, probe));
        MIB_EXPECT(layout.source == datadir::Source::UserLocal, "Program Files -> user-local");
        MIB_EXPECT(layout.dataDir == kUserRoot / "data", "data in %LOCALAPPDATA%/MIB_Studio_Qt/data");
        MIB_EXPECT(layout.logDir == kUserRoot / "logs", "log dir unchanged: %LOCALAPPDATA%/MIB_Studio_Qt/logs");
        MIB_EXPECT(layout.crashDir == kUserRoot / "crashes", "crash dir unchanged");
        MIB_EXPECT(layout.legacyDataDir == exe / "data", "legacy <exe dir>/data is the migration source");
        MIB_EXPECT(probe.calls.empty(), "Program Files is decided by path, not by probing");
        MIB_EXPECT(layout.reason.find("Program Files") != std::string::npos, "reason names Program Files");
    }
    // Program Files (x86), any case.
    {
        ProbeLog probe;
        const auto layout = datadir::resolve(windowsInputs("c:/PROGRAM FILES (X86)/MIB Studio Qt", probe));
        MIB_EXPECT(layout.source == datadir::Source::UserLocal, "Program Files (x86), any case -> user-local");
    }
    // A writable portable or dev folder keeps <exe dir>/data.
    {
        ProbeLog probe;
        const fs::path exe = "D:/tools/mib";
        const auto layout = datadir::resolve(windowsInputs(exe, probe));
        MIB_EXPECT(layout.source == datadir::Source::ExeDir, "writable exe dir -> exe-dir");
        MIB_EXPECT(layout.dataDir == exe / "data", "data in <exe dir>/data");
        MIB_EXPECT(layout.logDir == exe / "data" / "logs", "log beside the data");
        MIB_EXPECT(layout.crashDir == kUserRoot / "crashes", "Windows crash dir stays user-local");
        MIB_EXPECT(layout.legacyDataDir.empty(), "nothing to migrate");
        MIB_EXPECT(probe.calls.size() == 1 && probe.calls[0] == exe / "data", "probed <exe dir>/data once");
    }
    // An unwritable folder outside Program Files: the user-local dir.
    {
        ProbeLog probe;
        probe.answer = false;
        const fs::path exe = "D:/locked/mib";
        const auto layout = datadir::resolve(windowsInputs(exe, probe));
        MIB_EXPECT(layout.source == datadir::Source::UserLocal, "unwritable exe dir -> user-local");
        MIB_EXPECT(layout.dataDir == kUserRoot / "data", "data in %LOCALAPPDATA%");
        MIB_EXPECT(layout.logDir == kUserRoot / "logs", "user-local log dir");
        MIB_EXPECT(layout.legacyDataDir == exe / "data", "legacy dir set");
        MIB_EXPECT(layout.reason.find("not writable") != std::string::npos, "reason says not writable");
    }
    // %LOCALAPPDATA% unknown: nothing better than the old behaviour.
    {
        ProbeLog probe;
        auto in = windowsInputs("C:/Program Files/MIB Studio Qt", probe);
        in.localAppData.clear();
        const auto layout = datadir::resolve(in);
        MIB_EXPECT(layout.source == datadir::Source::ExeDir, "no LOCALAPPDATA -> exe-dir");
        MIB_EXPECT(layout.logDir == layout.dataDir / "logs", "and the log beside it");
        MIB_EXPECT(layout.crashDir == layout.dataDir / "crashes", "and the crash dir beside it");
        MIB_EXPECT(layout.reason.find("unknown") != std::string::npos, "reason says unknown");
    }
    // MIB_DATA_DIR wins everywhere and migrates nothing.
    {
        ProbeLog probe;
        auto in = windowsInputs("C:/Program Files/MIB Studio Qt", probe);
        in.overrideDir = "E:/bench/state";
        const auto layout = datadir::resolve(in);
        MIB_EXPECT(layout.source == datadir::Source::Override, "override wins");
        MIB_EXPECT(layout.dataDir == fs::path("E:/bench/state"), "override dir used");
        MIB_EXPECT(layout.logDir == fs::path("E:/bench/state") / "logs", "override keeps its log");
        MIB_EXPECT(layout.legacyDataDir.empty(), "override migrates nothing");
        MIB_EXPECT(probe.calls.empty(), "override does not probe");
    }
    // Not Windows: <exe dir>/data, never probed, as before (Linux dev and system builds).
    {
        ProbeLog probe;
        probe.answer = false;
        datadir::Inputs in;
        in.exeDir = "/opt/mib/bin";
        in.writable = probe.fn();
        const auto layout = datadir::resolve(in);
        MIB_EXPECT(layout.source == datadir::Source::ExeDir, "Linux -> exe-dir");
        MIB_EXPECT(layout.dataDir == fs::path("/opt/mib/bin") / "data", "Linux data dir unchanged");
        MIB_EXPECT(layout.logDir == layout.dataDir / "logs", "Linux log dir unchanged");
        MIB_EXPECT(layout.crashDir == layout.dataDir / "crashes", "Linux crash dir unchanged");
        MIB_EXPECT(probe.calls.empty(), "Linux does not probe");
        in.overrideDir = "/var/lib/mib";
        const auto overridden = datadir::resolve(in);
        MIB_EXPECT(overridden.dataDir == fs::path("/var/lib/mib") && overridden.crashDir == fs::path("/var/lib/mib") / "crashes",
                   "Linux override moves data and crash dirs");
    }

    // The Program Files test is by component, not by substring.
    MIB_EXPECT(datadir::isUnderProgramFiles("C:/Program Files/MIB Studio Qt"), "Program Files");
    MIB_EXPECT(datadir::isUnderProgramFiles("C:/Program Files (x86)/MIB"), "Program Files (x86)");
    MIB_EXPECT(!datadir::isUnderProgramFiles("C:/my program files backup/MIB"), "a lookalike folder");
    MIB_EXPECT(!datadir::isUnderProgramFiles("/home/u/Program Filesystem/mib"), "a prefix only");

    // logDirFor (AppBackend's side of the rule) agrees with resolve().
    MIB_EXPECT(datadir::logDirFor(kUserRoot / "data", kLocalAppData, true) == kUserRoot / "logs",
               "user-local data -> user-local logs");
    MIB_EXPECT(datadir::logDirFor(fs::path("c:/users/KENNY/appdata/local/mib_studio_qt/data/"), kLocalAppData, true) ==
                   kUserRoot / "logs",
               "matched case-insensitively, trailing separator ignored, on Windows");
    MIB_EXPECT(datadir::logDirFor("C:/Program Files/MIB Studio Qt/data", kLocalAppData, true) == kUserRoot / "logs",
               "a caller still passing the Program Files dir logs user-local (the old rule)");
    MIB_EXPECT(datadir::logDirFor("D:/tools/mib/data", kLocalAppData, true) == fs::path("D:/tools/mib/data") / "logs",
               "other dirs log beside the data");
    MIB_EXPECT(datadir::logDirFor(kUserRoot / "data", kLocalAppData, false) == kUserRoot / "data" / "logs",
               "not Windows: beside the data");
    for (const auto source : {datadir::Source::Override, datadir::Source::ExeDir, datadir::Source::UserLocal})
        MIB_EXPECT(std::string(datadir::sourceName(source)) != "unknown", "every source is named");
}

void probeAndEnvironment(const mib::test::TempDir& scratch) {
    // The real probe creates the folder and leaves no probe file behind.
    const fs::path fresh = scratch / "probe" / "nested" / "data";
    MIB_EXPECT(datadir::probeWritable(fresh), "a writable folder probes writable");
    MIB_EXPECT(fs::is_directory(fresh), "the probe creates the folder");
    MIB_EXPECT(fs::is_empty(fresh), "no probe file left behind");
    if (permissionFaultsWork()) {
        const fs::path locked = scratch / "locked";
        fs::create_directories(locked);
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
        MIB_EXPECT(!datadir::probeWritable(locked / "data"), "a read-only parent probes unwritable");
        MIB_EXPECT(!datadir::probeWritable(locked), "a read-only folder probes unwritable");
        fs::permissions(locked, fs::perms::owner_all);
    }

    // currentInputs reads MIB_DATA_DIR and the platform.
    unsetEnv(datadir::kDataDirEnv);
    auto in = datadir::currentInputs(scratch.path());
    MIB_EXPECT(in.exeDir == scratch.path(), "exe dir passed through");
    MIB_EXPECT(in.overrideDir.empty(), "no override when MIB_DATA_DIR is unset");
    MIB_EXPECT(static_cast<bool>(in.writable), "the real probe is wired");
#ifdef _WIN32
    MIB_EXPECT(in.windows && !in.localAppData.empty(), "Windows: %LOCALAPPDATA% resolved");
    MIB_EXPECT(datadir::localAppDataDir() == in.localAppData, "one LOCALAPPDATA lookup");
#else
    MIB_EXPECT(!in.windows && in.localAppData.empty(), "not Windows: no LOCALAPPDATA");
    MIB_EXPECT(datadir::localAppDataDir().empty(), "not Windows: lookup is empty");
#endif
    const std::string overrideDir = (scratch / "override").string();
    setEnv(datadir::kDataDirEnv, overrideDir.c_str());
    in = datadir::currentInputs(scratch.path());
    MIB_EXPECT(in.overrideDir == fs::path(overrideDir), "MIB_DATA_DIR read");
    MIB_EXPECT(datadir::resolve(in).dataDir == fs::path(overrideDir), "and it wins");
    unsetEnv(datadir::kDataDirEnv);

    // The string AppBackend takes round-trips through std::filesystem::path.
    const fs::path sample = scratch / "MIB_Studio_Qt" / "data";
    MIB_EXPECT(fs::path(datadir::toBackendString(sample)) == sample, "backend string round-trips");
}

// A legacy dir with every kind of entry the migration must copy or skip.
void seedLegacy(const fs::path& legacy) {
    writeText(legacy / "instrument_identity.json", "{\"schema\":1,\"instrument_id\":\"123e4567-e89b-42d3-a456-426614174000\"}\n");
    writeText(legacy / "instrument_calibration.json", "{\"records\":[1,2,3]}\n");
    writeText(legacy / "app.sqlite3", std::string("SQLite format 3\0binary\xff\x01", 24));
    writeText(legacy / "profile_registry" / "cache.json", "{\"revisions\":[]}");
    writeText(legacy / "methods" / "rev-1" / "config.json", "{\"m\":1}");
    writeText(legacy / "logs" / "app.log", "old log\n");
    writeText(legacy / "mock_frames" / "frame_0.tiff", "tiff");
    writeText(legacy / "pipeline_timing" / "t.csv", "a,b\n");
}

void migrationRoundTrip(const mib::test::TempDir& scratch) {
    const fs::path legacy = scratch / "rt" / "Program Files" / "MIB Studio Qt" / "data";
    const fs::path fresh = scratch / "rt" / "LocalAppData" / "MIB_Studio_Qt" / "data";
    seedLegacy(legacy);

    const auto result = datadir::migrateLegacyState(legacy, fresh);
    MIB_EXPECT(result.attempted, "an empty new dir and a legacy dir with state -> migrate");
    MIB_EXPECT(result.failed.empty(), "nothing failed");
    for (const char* entry : {"instrument_identity.json", "instrument_calibration.json", "app.sqlite3",
                              "profile_registry", "methods"})
        MIB_EXPECT(contains(result.copied, entry), std::string("copied ") + entry);
    MIB_EXPECT(result.copied.size() == 5, "only the state entries present were copied");
    for (const char* file : {"instrument_identity.json", "instrument_calibration.json", "app.sqlite3",
                             "profile_registry/cache.json", "methods/rev-1/config.json"})
        MIB_EXPECT(readText(fresh / file) == readText(legacy / file), std::string("byte-identical ") + file);
    for (const char* skipped : {"logs", "mock_frames", "pipeline_timing"})
        MIB_EXPECT(!fs::exists(fresh / skipped), std::string("not state, not copied: ") + skipped);
    for (const auto& item : fs::directory_iterator(fresh))
        MIB_EXPECT(item.path().filename().string().rfind(".", 0) != 0, "no staging entry left behind");
    // The legacy dir is never changed.
    MIB_EXPECT(readText(legacy / "instrument_identity.json").find("123e4567") != std::string::npos &&
                   fs::exists(legacy / "methods" / "rev-1" / "config.json") && fs::exists(legacy / "logs" / "app.log"),
               "legacy files kept");
    MIB_EXPECT(datadir::hasState(fresh), "the new dir now holds state");

    // Once only: a second start migrates nothing, even after the legacy dir changed.
    writeText(legacy / "instrument_calibration.json", "{\"records\":[\"newer, written elevated\"]}\n");
    const auto again = datadir::migrateLegacyState(legacy, fresh);
    MIB_EXPECT(!again.attempted && again.copied.empty(), "second start: nothing copied");
    MIB_EXPECT(again.note.find("already holds state") != std::string::npos, "note says why");
    MIB_EXPECT(readText(fresh / "instrument_calibration.json") == "{\"records\":[1,2,3]}\n",
               "the new dir's state is never overwritten");
}

void migrationSkips(const mib::test::TempDir& scratch) {
    // A new dir with any state is left alone.
    {
        const fs::path legacy = scratch / "skip1" / "legacy";
        const fs::path fresh = scratch / "skip1" / "fresh";
        seedLegacy(legacy);
        writeText(fresh / "instrument_identity.json", "{\"schema\":1,\"instrument_id\":\"mine\"}");
        const auto result = datadir::migrateLegacyState(legacy, fresh);
        MIB_EXPECT(!result.attempted, "a new dir with state is not migrated into");
        MIB_EXPECT(!fs::exists(fresh / "instrument_calibration.json"), "no entry added to it");
        MIB_EXPECT(readText(fresh / "instrument_identity.json").find("mine") != std::string::npos, "its identity kept");
    }
    // No legacy dir, or one without state.
    {
        const auto missing = datadir::migrateLegacyState(scratch / "skip2" / "absent", scratch / "skip2" / "fresh");
        MIB_EXPECT(!missing.attempted && missing.note.find("no legacy data dir") != std::string::npos, "absent legacy dir");
        MIB_EXPECT(!fs::exists(scratch / "skip2" / "fresh"), "nothing created for an absent legacy dir");
        const fs::path logsOnly = scratch / "skip3" / "legacy";
        writeText(logsOnly / "logs" / "app.log", "x");
        const auto empty = datadir::migrateLegacyState(logsOnly, scratch / "skip3" / "fresh");
        MIB_EXPECT(!empty.attempted && empty.note.find("holds no state") != std::string::npos,
                   "a legacy dir with only logs/ (the rig's case) migrates nothing");
        const auto none = datadir::migrateLegacyState(fs::path(), scratch / "skip3" / "fresh");
        MIB_EXPECT(!none.attempted, "no legacy dir given");
        const auto same = datadir::migrateLegacyState(logsOnly, logsOnly / ".");
        MIB_EXPECT(!same.attempted && same.note.find("is the data dir") != std::string::npos, "same dir");
    }
    // A staging leftover from an interrupted start is not state and is replaced.
    {
        const fs::path legacy = scratch / "skip4" / "legacy";
        const fs::path fresh = scratch / "skip4" / "fresh";
        writeText(legacy / "instrument_identity.json", "legacy-id");
        writeText(fresh / ".instrument_identity.json.migrating", "half");
        const auto result = datadir::migrateLegacyState(legacy, fresh);
        MIB_EXPECT(result.attempted && contains(result.copied, "instrument_identity.json"), "leftover ignored");
        MIB_EXPECT(readText(fresh / "instrument_identity.json") == "legacy-id", "full copy in place");
        MIB_EXPECT(!fs::exists(fresh / ".instrument_identity.json.migrating"), "leftover gone");
    }
}

void migrationFaults(const mib::test::TempDir& scratch) {
    if (!permissionFaultsWork()) {
        std::fprintf(stderr, "SKIP migration permission faults (Windows or root)\n");
        return;
    }
    // One unreadable entry fails alone: no partial file, the others still copied,
    // the legacy entry kept.
    {
        const fs::path legacy = scratch / "fault1" / "legacy";
        const fs::path fresh = scratch / "fault1" / "fresh";
        seedLegacy(legacy);
        fs::permissions(legacy / "instrument_calibration.json", fs::perms::none);
        fs::permissions(legacy / "methods" / "rev-1" / "config.json", fs::perms::none);
        const auto result = datadir::migrateLegacyState(legacy, fresh);
        fs::permissions(legacy / "instrument_calibration.json", fs::perms::owner_read | fs::perms::owner_write);
        fs::permissions(legacy / "methods" / "rev-1" / "config.json", fs::perms::owner_read | fs::perms::owner_write);
        MIB_EXPECT(result.attempted, "attempted");
        MIB_EXPECT(contains(result.copied, "instrument_identity.json") && contains(result.copied, "app.sqlite3") &&
                       contains(result.copied, "profile_registry"),
                   "readable entries copied");
        MIB_EXPECT(result.failed.size() == 2, "the unreadable file and the folder holding one failed");
        MIB_EXPECT(!fs::exists(fresh / "instrument_calibration.json") && !fs::exists(fresh / "methods"),
                   "no partial file or folder in place");
        for (const auto& item : fs::directory_iterator(fresh))
            MIB_EXPECT(item.path().filename().string().rfind(".", 0) != 0, "no staging entry left after a failure");
        MIB_EXPECT(fs::exists(legacy / "instrument_calibration.json"), "legacy entry kept");
    }
    // The new data dir cannot be created: every entry fails, nothing thrown.
    {
        const fs::path legacy = scratch / "fault2" / "legacy";
        const fs::path parent = scratch / "fault2" / "readonly";
        writeText(legacy / "instrument_identity.json", "id");
        fs::create_directories(parent);
        fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec);
        const auto result = datadir::migrateLegacyState(legacy, parent / "data");
        fs::permissions(parent, fs::perms::owner_all);
        MIB_EXPECT(result.attempted && result.copied.empty() && result.failed.size() == 1,
                   "an uncreatable data dir fails every entry without throwing");
    }
}

// AppBackend migrates before it loads the identity: the instrument keeps its id.
void appBackendMigrates(const mib::test::TempDir& scratch) {
    setEnv("MIB_CAMERA_MODE", "mock");
    setEnv("MIB_DISABLED_SERVICES", "capture,autofocus,trigger,dot_grid,playback,processing,hdf5");
    setEnv("MIB_STUDIO_EMODULUS_LUT_MANIFEST_URL", "file:///nonexistent/mib-lut-manifest.json");
    const fs::path legacy = scratch / "app" / "exe" / "data";
    const fs::path fresh = scratch / "app" / "user" / "data";
    const std::string id = "123e4567-e89b-42d3-a456-426614174000";
    writeText(legacy / "instrument_identity.json", "{\"schema\":1,\"instrument_id\":\"" + id + "\"}\n");
    writeText(legacy / "instrument_calibration.json", "{\"schema\":1,\"records\":[]}\n");

    backend::AppBackend app;
    app.setDataDirOrigin("user-local: test", legacy);
    MIB_REQUIRE(app.initialize(fresh.string()), "initialize");
    MIB_EXPECT(app.instrumentIdentity().id == id, "the migrated identity is the instrument's id");
    MIB_EXPECT(fs::exists(fresh / "instrument_calibration.json"), "calibration migrated");
    MIB_EXPECT(app.logDir() == fresh / "logs", "log dir follows the rule (not Windows-user-local here)");
    MIB_EXPECT(fs::exists(legacy / "instrument_identity.json"), "legacy identity kept");
    app.shutdown();
}

} // namespace

int main() {
    mib::test::Watchdog watchdog(60);
    mib::test::TempDir scratch("mib_data_directory");
    resolverRules();
    probeAndEnvironment(scratch);
    migrationRoundTrip(scratch);
    migrationSkips(scratch);
    migrationFaults(scratch);
    appBackendMigrates(scratch);
    return mib::test::exitCode();
}
