// readiness_probe_cache_test (#713, finding 5)
//
// The readiness destination probes write to the disk: storage.output creates
// and removes a temp file next to the target, storage.roundtrip writes and
// reads back a small HDF5 file. They run once per (output path, readiness
// generation), so a UI polling readiness does not churn the SSD:
//  - path-less Prepare polling never probes the cwd or a previously chosen
//    destination, with either a cold or warm destination cache;
//  - repeated evaluations with the same path and generation create no file in
//    the destination after the first (its modification time stays put: a file
//    created or removed in a directory moves it) and leave nothing behind;
//  - the cached evaluations give the same gates as the probed one;
//  - a path change probes the new destination, a return to the old path
//    probes it again, and so does any other generation bump with the same path;
//  - the metadata checks (no writes) still run every time: a destination
//    parent replaced by a file fails at once;
//  - only a pass is kept: a destination that failed the write probe and is
//    made writable passes at the next evaluation (where permissions are
//    enforced).

#include "backend/app/AppBackend.h"
#include "backend/app/ExperimentCoordinator.h"
#include "backend/processing/ProcessingService.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using backend::app::GateStatus;
namespace fs = std::filesystem;

namespace {
GateStatus statusOf(const backend::app::ExperimentReadinessSnapshot& r, const char* id) {
    const auto* g = r.gate(id);
    return g ? g->status : GateStatus::Unavailable;
}

size_t entries(const fs::path& dir) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

// Set the directory's modification time an hour back and return it: any file a
// probe creates or removes in it afterwards gives a different time.
fs::file_time_type age(const fs::path& dir) {
    fs::last_write_time(dir, fs::last_write_time(dir) - std::chrono::hours(1));
    return fs::last_write_time(dir);
}

bool sameGate(const backend::app::ExperimentReadinessSnapshot& a,
              const backend::app::ExperimentReadinessSnapshot& b, const char* id) {
    const auto* x = a.gate(id);
    const auto* y = b.gate(id);
    return x && y && x->status == y->status && x->reason == y->reason &&
           x->remediation == y->remediation && x->detail == y->detail;
}
} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("MIB_DISABLED_SERVICES",
              "auto_update,autofocus,trigger,syringe_pump,pulse_generator,capture");
#else
    setenv("MIB_DISABLED_SERVICES",
           "auto_update,autofocus,trigger,syringe_pump,pulse_generator,capture", 1);
#endif
    mib::test::Watchdog wd(60);
    mib::test::TempDir td("readiness_probe_cache");
    backend::AppBackend backend;
    MIB_REQUIRE(backend.initialize((td.path() / "data").string()), "backend init");
    auto& coord = backend.experiment();

    const fs::path dest = td.path() / "dest";
    const fs::path dest2 = td.path() / "dest2";
    MIB_REQUIRE(fs::create_directories(dest) && fs::create_directories(dest2),
                "destination directories");
    const std::string out1 = (dest / "run1.h5").string();
    const std::string out2 = (dest2 / "run2.h5").string();

    // A missing path must not become a relative destination or reuse the last
    // probe path. Watch directory mtime as well as entries: a transient probe
    // that creates and deletes a file must still be detected.
    const auto noPathDoesNotProbe = [&] {
        const auto oldCwd = fs::current_path();
        struct RestoreCwd {
            fs::path path;
            ~RestoreCwd() { fs::current_path(path); }
        } restore{oldCwd};
        fs::current_path(dest2);
        const auto destinationTime = age(dest);
        const auto cwdTime = age(dest2);
        for (int i = 0; i < 20; ++i) {
            const auto r = coord.evaluateReadiness("");
            MIB_EXPECT(r.candidate.outputPath.empty(), "path-less readiness keeps output absent");
            MIB_EXPECT(statusOf(r, "storage.output") == GateStatus::Unavailable,
                       "path-less storage is unavailable, never a fabricated pass");
            MIB_EXPECT(r.gate("storage.roundtrip") == nullptr,
                       "path-less readiness does not claim a storage round trip");
        }
        MIB_EXPECT(fs::last_write_time(dest) == destinationTime,
                   "path-less polling never probes the previous destination");
        MIB_EXPECT(fs::last_write_time(dest2) == cwdTime,
                   "path-less polling never probes the working directory");
        MIB_EXPECT(entries(dest) == 0 && entries(dest2) == 0,
                   "path-less polling leaves no files behind");
    };
    wd.mark("path-less cold cache");
    noPathDoesNotProbe();

    // ---- 1. Cold cache: the first evaluation probes ---------------------------
    wd.mark("cold");
    auto aged = age(dest);
    const auto first = coord.evaluateReadiness(out1);
    MIB_EXPECT(statusOf(first, "storage.output") == GateStatus::Pass,
               "writable destination passes");
    MIB_EXPECT(statusOf(first, "storage.roundtrip") == GateStatus::Pass, "HDF5 round trip passes");
    MIB_EXPECT(fs::last_write_time(dest) != aged, "the first evaluation probes the destination");
    MIB_EXPECT(entries(dest) == 0, "the probes leave nothing behind");

    // ---- 2. Same path and generation: no file is created ----------------------
    wd.mark("cached");
    aged = age(dest);
    for (int i = 0; i < 20; ++i) {
        const auto r = coord.evaluateReadiness(out1);
        MIB_EXPECT(r.generation == first.generation, "nothing changed: same generation");
        MIB_EXPECT(sameGate(r, first, "storage.output") && sameGate(r, first, "storage.roundtrip"),
                   "the cached gates are the probed ones");
    }
    MIB_EXPECT(fs::last_write_time(dest) == aged,
               "repeated evaluations with the same path and generation create no file");
    MIB_EXPECT(entries(dest) == 0, "still nothing in the destination");

    wd.mark("path-less warm cache");
    noPathDoesNotProbe();

    // ---- 3. A path change probes the new destination -------------------------
    wd.mark("path change");
    auto aged2 = age(dest2);
    aged = age(dest);
    const auto moved = coord.evaluateReadiness(out2);
    MIB_EXPECT(moved.generation != first.generation, "a path change bumps the generation");
    MIB_EXPECT(fs::last_write_time(dest2) != aged2, "the new path is probed");
    MIB_EXPECT(fs::last_write_time(dest) == aged, "the old path is not");
    MIB_EXPECT(statusOf(moved, "storage.output") == GateStatus::Pass &&
                   statusOf(moved, "storage.roundtrip") == GateStatus::Pass,
               "the new destination passes");
    aged2 = age(dest2);
    MIB_EXPECT(coord.evaluateReadiness(out2).generation == moved.generation &&
                   fs::last_write_time(dest2) == aged2,
               "and is then cached");
    const auto back = coord.evaluateReadiness(out1);
    MIB_EXPECT(back.generation != moved.generation && fs::last_write_time(dest) != aged,
               "returning to the first path probes it again");
    MIB_EXPECT(entries(dest) == 0 && entries(dest2) == 0, "no probe file left behind");

    // ---- 4. Any other generation bump with the same path re-probes -------------
    wd.mark("generation bump");
    auto& proc = backend.processing();
    const auto budget = proc.getMaxBufferedBytes();
    aged = age(dest);
    proc.setMaxBufferedBytes(budget + (1ULL << 20));
    const auto bumped = coord.evaluateReadiness(out1);
    MIB_EXPECT(bumped.generation != back.generation, "the buffer budget bumps the generation");
    MIB_EXPECT(fs::last_write_time(dest) != aged, "a new generation probes the same path again");
    proc.setMaxBufferedBytes(budget);

    // ---- 5. The metadata checks still run every time ---------------------------
    wd.mark("metadata");
    const auto settled = coord.evaluateReadiness(out1);
    MIB_EXPECT(statusOf(settled, "storage.output") == GateStatus::Pass, "pass before the change");
    fs::remove_all(dest);
    {
        std::ofstream f(dest);
        f << "x";
    }
    const auto blocked = coord.evaluateReadiness(out1);
    MIB_EXPECT(blocked.generation == settled.generation, "the key did not change");
    MIB_EXPECT(statusOf(blocked, "storage.output") == GateStatus::Fail,
               "a parent replaced by a file fails without a generation bump");

    // ---- 6. Only a pass is kept: a destination made writable passes at once ----
    // Needs permissions that are enforced (not root, not the Windows read-only
    // attribute): checked by trying to write in the read-only directory first.
    wd.mark("failure retried");
    const auto writable = fs::status(dest2).permissions();
    fs::permissions(dest2, fs::perms::owner_read | fs::perms::owner_exec);
    bool enforced = false;
    {
        const auto canary = dest2 / "canary";
        {
            std::ofstream f(canary);
            enforced = !f.good();
        }
        std::error_code ec;
        fs::remove(canary, ec);
    }
    if (enforced) {
        proc.setMaxBufferedBytes(budget + (2ULL << 20)); // a new generation probes again
        const auto denied = coord.evaluateReadiness(out2);
        MIB_EXPECT(statusOf(denied, "storage.output") == GateStatus::Fail,
                   "a read-only destination fails the write probe");
        fs::permissions(dest2, writable);
        const auto fixed = coord.evaluateReadiness(out2);
        MIB_EXPECT(fixed.generation == denied.generation, "nothing in the key changed");
        MIB_EXPECT(
            statusOf(fixed, "storage.output") == GateStatus::Pass &&
                statusOf(fixed, "storage.roundtrip") == GateStatus::Pass,
            "the failure was not kept: the fixed destination passes without a generation bump");
        aged2 = age(dest2);
        coord.evaluateReadiness(out2);
        MIB_EXPECT(fs::last_write_time(dest2) == aged2, "and its pass is kept");
        proc.setMaxBufferedBytes(budget);
    } else {
        std::fprintf(stderr, "skip: directory permissions are not enforced here\n");
    }
    fs::permissions(dest2, writable);

    backend.shutdown();
    return mib::test::exitCode();
}
