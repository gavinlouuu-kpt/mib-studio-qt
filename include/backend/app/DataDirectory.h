#pragma once

// Where MIB Studio keeps its per-instrument state: the data dir (rig check
// v1-02 finding F1). One rule, resolved once at startup by the Qt shell:
//
//   1. MIB_DATA_DIR, when set and not empty, wins on every platform.
//   2. On Windows, when the exe sits under a "Program Files" folder, or
//      <exe dir>/data cannot be written, the data dir is
//      %LOCALAPPDATA%/MIB_Studio_Qt/data. The Program Files test is by path,
//      so an elevated and a normal launch of an installed app use the same
//      folder.
//   3. Otherwise <exe dir>/data, as before (dev builds, portable copies,
//      Linux).
//
// The log, crash and migration paths derive from the same inputs here, so no
// other file repeats the rule. Qt-free: the backend and the shell share it.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace backend::app::datadir {

// Folder under %LOCALAPPDATA% shared by the log, crash database, config
// (include/) and, for installed builds, the data dir.
inline constexpr const char* kUserFolderName = "MIB_Studio_Qt";
// Environment override for the data dir (all platforms); empty means unset.
inline constexpr const char* kDataDirEnv = "MIB_DATA_DIR";

enum class Source {
    Override,  // MIB_DATA_DIR
    ExeDir,    // <exe dir>/data
    UserLocal, // %LOCALAPPDATA%/MIB_Studio_Qt/data
};

const char* sourceName(Source source);

struct Inputs {
    std::filesystem::path exeDir;
    // %LOCALAPPDATA% on Windows; empty when the lookup failed or off Windows.
    std::filesystem::path localAppData;
    // MIB_DATA_DIR; empty when unset.
    std::filesystem::path overrideDir;
    bool windows = false;
    // Whether files can be created in this directory (creating it if needed).
    // Called only on Windows, and only when the exe is not under Program Files.
    std::function<bool(const std::filesystem::path&)> writable;
};

struct Layout {
    std::filesystem::path dataDir;
    std::filesystem::path logDir;
    std::filesystem::path crashDir;
    // <exe dir>/data when the state moved away from it (the migration
    // source); empty otherwise.
    std::filesystem::path legacyDataDir;
    Source source = Source::ExeDir;
    std::string reason; // one line for the log
};

// The rule above, on injected inputs (tests drive every branch on any OS).
Layout resolve(const Inputs& inputs);

// The running process: the exe dir given by the shell, %LOCALAPPDATA% from
// the shell known-folder API, MIB_DATA_DIR, and probeWritable().
Inputs currentInputs(const std::filesystem::path& exeDir);

// Where app.log goes for a data dir: %LOCALAPPDATA%/MIB_Studio_Qt/logs when
// the data dir is the user-local one or still under Program Files (a legacy
// caller), else <data dir>/logs. AppBackend and resolve() both use it.
std::filesystem::path logDirFor(const std::filesystem::path& dataDir,
                                const std::filesystem::path& localAppData, bool windows);
// The same for the running process.
std::filesystem::path logDirFor(const std::filesystem::path& dataDir);

// True when a component of the path is "Program Files" or
// "Program Files (x86)" (case-insensitive).
bool isUnderProgramFiles(const std::filesystem::path& dir);

// Creates the directory when missing, then writes and removes a probe file.
bool probeWritable(const std::filesystem::path& dir);

// %LOCALAPPDATA% on Windows (CSIDL_LOCAL_APPDATA, wide API); empty elsewhere
// or when the lookup fails.
std::filesystem::path localAppDataDir();

// The data dir as the std::string AppBackend takes (the native narrow
// encoding std::filesystem::path(std::string) reads back). Falls back to
// UTF-8 when the native code page cannot represent the path.
std::string toBackendString(const std::filesystem::path& path);

// ---- One-time migration of the legacy <exe dir>/data state ----

// The entries (files and folders, relative to a data dir) that hold state
// worth keeping: identity, calibration, the SQLite database, the profile
// registry cache and methods, and the smaller per-instrument records. Logs,
// crash dumps, timing dumps and mock frames are not state.
const std::vector<std::string>& stateEntries();

// True when any state entry exists in the directory.
bool hasState(const std::filesystem::path& dataDir);

struct MigrationResult {
    bool attempted = false;           // the new dir had no state, the legacy dir had some
    std::vector<std::string> copied;  // entry names copied
    std::vector<std::string> failed;  // "entry: reason"
    std::string note;                 // why nothing was copied, when attempted is false
};

// When dataDir has no state and legacyDir has some, copies every state entry
// from legacyDir into dataDir. Each entry is copied under a temporary name and
// renamed into place, so a failed copy leaves no partial state. Never deletes
// or changes anything in legacyDir, never overwrites in dataDir.
MigrationResult migrateLegacyState(const std::filesystem::path& legacyDir,
                                   const std::filesystem::path& dataDir);

} // namespace backend::app::datadir
