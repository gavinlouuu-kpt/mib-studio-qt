#include "backend/app/DataDirectory.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace backend::app::datadir {
namespace {
namespace fs = std::filesystem;

std::string utf8(const fs::path& path)
{
#if defined(__cpp_lib_char8_t)
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.u8string();
#endif
}

std::string lowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Lexical comparison; case-insensitive on Windows, where the file system is.
bool sameLocation(const fs::path& a, const fs::path& b, bool windows)
{
    const std::string left = utf8(a.lexically_normal());
    const std::string right = utf8(b.lexically_normal());
    auto trim = [](std::string s) {
        while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        return s;
    };
    return windows ? lowerAscii(trim(left)) == lowerAscii(trim(right)) : trim(left) == trim(right);
}

bool entryExists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(fs::symlink_status(path, ec));
}

} // namespace

const char* sourceName(Source source)
{
    switch (source)
    {
    case Source::Override: return "override";
    case Source::ExeDir: return "exe-dir";
    case Source::UserLocal: return "user-local";
    }
    return "unknown";
}

bool isUnderProgramFiles(const fs::path& dir)
{
    for (const auto& component : dir)
    {
        const std::string name = lowerAscii(utf8(component));
        if (name == "program files" || name == "program files (x86)") return true;
        // The 8.3 short names (C:\PROGRA~1, C:\PROGRA~2), when the exe was started through one.
        if (name.size() == 8 && name.compare(0, 7, "progra~") == 0 && std::isdigit(static_cast<unsigned char>(name[7])))
            return true;
    }
    return false;
}

bool probeWritable(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) return false;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path probe = dir / (".mib-write-probe-" + std::to_string(stamp));
    bool written = false;
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (out)
        {
            out << "probe";
            out.flush();
            written = static_cast<bool>(out);
        }
    } // closed before the remove (Windows)
    fs::remove(probe, ec);
    return written;
}

bool isAsciiPath(const fs::path& path)
{
    const std::string text = utf8(path);
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

fs::path localAppDataDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buffer)))
        return {};
    const fs::path longPath(buffer);
    if (isAsciiPath(longPath)) return longPath;
    // A non-ASCII account name (C:\Users\<name>): its 8.3 short name is ASCII when the volume
    // keeps short names (the system drive does by default).
    const std::wstring wide = longPath.wstring();
    const DWORD needed = GetShortPathNameW(wide.c_str(), nullptr, 0);
    if (needed > 0)
    {
        std::wstring shortPath(needed, L'\0');
        const DWORD written = GetShortPathNameW(wide.c_str(), shortPath.data(), needed);
        if (written > 0 && written < needed)
        {
            shortPath.resize(written);
            const fs::path candidate(shortPath);
            if (isAsciiPath(candidate)) return candidate;
        }
    }
    return longPath;
#else
    return {};
#endif
}

std::string toBackendString(const fs::path& path)
{
    return utf8(path);
}

fs::path logDirFor(const fs::path& dataDir, const fs::path& localAppData, bool windows)
{
    if (windows && !localAppData.empty())
    {
        const fs::path userRoot = localAppData / kUserFolderName;
        if (isUnderProgramFiles(dataDir) || sameLocation(dataDir, userRoot / "data", windows))
            return userRoot / "logs";
    }
    return dataDir / "logs";
}

fs::path logDirFor(const fs::path& dataDir)
{
#ifdef _WIN32
    return logDirFor(dataDir, localAppDataDir(), true);
#else
    return logDirFor(dataDir, fs::path(), false);
#endif
}

Layout resolve(const Inputs& in)
{
    Layout out;
    const bool knowsUserDir = in.windows && !in.localAppData.empty();
    // AppBackend reads its data dir as a narrow string, partly as UTF-8 and partly in the ANSI
    // code page (#762): only an ASCII path reads the same both ways.
    const bool userDirUsable = knowsUserDir && isAsciiPath(in.localAppData);
    const fs::path userRoot = knowsUserDir ? in.localAppData / kUserFolderName : fs::path();
    const fs::path exeData = in.exeDir / "data";

    if (!in.overrideDir.empty())
    {
        out.dataDir = in.overrideDir;
        out.source = Source::Override;
        out.reason = std::string(kDataDirEnv) + " is set";
    }
    else if (userDirUsable && isUnderProgramFiles(in.exeDir))
    {
        out.dataDir = userRoot / "data";
        out.legacyDataDir = exeData;
        out.source = Source::UserLocal;
        out.reason = "installed under Program Files, which a normal user cannot write";
    }
    else if (userDirUsable && in.writable && !in.writable(exeData))
    {
        out.dataDir = userRoot / "data";
        out.legacyDataDir = exeData;
        out.source = Source::UserLocal;
        out.reason = "<exe dir>/data is not writable";
    }
    else
    {
        out.dataDir = exeData;
        out.source = Source::ExeDir;
        if (in.windows && !knowsUserDir && isUnderProgramFiles(in.exeDir))
            out.reason = "%LOCALAPPDATA% is unknown, kept <exe dir>/data";
        else if (knowsUserDir && !userDirUsable && isUnderProgramFiles(in.exeDir))
            out.reason = "%LOCALAPPDATA% (" + utf8(in.localAppData) +
                         ") has no ASCII form, kept <exe dir>/data (state is not saved)";
        else
            out.reason = in.windows ? "<exe dir>/data is writable" : "<exe dir>/data (not Windows)";
    }

    out.logDir = logDirFor(out.dataDir, in.localAppData, in.windows);
    out.crashDir = knowsUserDir ? userRoot / "crashes" : out.dataDir / "crashes";
    return out;
}

Inputs currentInputs(const fs::path& exeDir)
{
    Inputs in;
    in.exeDir = exeDir;
#ifdef _WIN32
    in.windows = true;
    in.localAppData = localAppDataDir();
#endif
    if (const char* value = std::getenv(kDataDirEnv); value && *value) in.overrideDir = fs::path(value);
    in.writable = probeWritable;
    return in;
}

const std::vector<std::string>& stateEntries()
{
    // Every file or folder AppBackend::initialize and the services it wires
    // keep under the data dir. A new one belongs here too, or an upgrade over
    // a build that wrote it under <exe dir>/data loses it.
    static const std::vector<std::string> entries = {
        "instrument_identity.json",
        "instrument_calibration.json",
        "app.sqlite3",
        "app.sqlite3-journal",
        "app.sqlite3-wal",
        "app.sqlite3-shm",
        "profile_registry",
        "methods",
        "startup_configuration.json",
        "stage_reference.json",
        "stage_limits_verified.json",
        "instrument_run_window.json",
        "ssd-state.json",
        "camera",
        "config",
    };
    return entries;
}

bool hasState(const fs::path& dataDir)
{
    return std::any_of(stateEntries().begin(), stateEntries().end(),
                       [&](const std::string& entry) { return entryExists(dataDir / entry); });
}

MigrationResult migrateLegacyState(const fs::path& legacyDir, const fs::path& dataDir)
{
    MigrationResult result;
    std::error_code ec;
#ifdef _WIN32
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    if (legacyDir.empty())
    {
        result.note = "no legacy data dir";
        return result;
    }
    if (sameLocation(legacyDir, dataDir, windows))
    {
        result.note = "the legacy data dir is the data dir";
        return result;
    }
    if (!fs::is_directory(legacyDir, ec))
    {
        result.note = "no legacy data dir at " + utf8(legacyDir);
        return result;
    }
    const fs::path pending = dataDir / kMigrationPendingMarker;
    const bool resuming = entryExists(pending);
    if (!resuming && hasState(dataDir))
    {
        result.note = "the data dir already holds state";
        return result;
    }
    std::vector<std::string> present;
    for (const auto& entry : stateEntries())
        if (entryExists(legacyDir / entry)) present.push_back(entry);
    if (present.empty())
    {
        result.note = "the legacy data dir " + utf8(legacyDir) + " holds no state";
        if (resuming) fs::remove(pending, ec);
        return result;
    }

    result.attempted = true;
    fs::create_directories(dataDir, ec);
    if (ec)
    {
        for (const auto& entry : present) result.failed.push_back(entry + ": cannot create the data dir: " + ec.message());
        return result;
    }
    // Written before the first copy and removed once every entry is in place: a start that
    // failed or was cut off part-way is resumed (the entries still missing are copied) instead
    // of being taken for a finished migration.
    if (!resuming)
    {
        std::ofstream marker(pending, std::ios::binary | std::ios::trunc);
        marker << "copying the state of " << utf8(legacyDir) << "\n";
        if (!marker)
        {
            for (const auto& entry : present) result.failed.push_back(entry + ": cannot write to the data dir");
            return result;
        }
    }
    for (const auto& entry : present)
    {
        const fs::path source = legacyDir / entry;
        const fs::path target = dataDir / entry;
        const fs::path staging = dataDir / ("." + entry + ".migrating");
        std::error_code error;
        fs::remove_all(staging, error); // a leftover from an interrupted start
        error.clear();
        if (entryExists(target))
        {
            result.kept.push_back(entry); // copied by an earlier start, or the data dir's own
            continue;
        }
        const auto status = fs::status(source, error); // a symbolic link: its target is copied
        if (!error && fs::is_regular_file(status))
            fs::copy_file(source, staging, fs::copy_options::overwrite_existing, error);
        else if (!error && fs::is_directory(status))
            fs::copy(source, staging, fs::copy_options::recursive, error);
        else if (!error)
        {
            result.failed.push_back(entry + ": not a regular file or folder");
            continue;
        }
        if (!error) fs::rename(staging, target, error);
        if (error)
        {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            result.failed.push_back(entry + ": " + error.message());
            continue;
        }
        result.copied.push_back(entry);
    }
    if (result.failed.empty()) fs::remove(pending, ec);
    return result;
}

} // namespace backend::app::datadir
