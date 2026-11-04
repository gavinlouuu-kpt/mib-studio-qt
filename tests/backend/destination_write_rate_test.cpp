// destination_write_rate_test (#841): a slow write-rate sample is measured again.
// On the bench PC one of nine probes of a healthy NVMe read 15.2 MB/s (it ran into the 0.75 s
// bound) against 469-700 MB/s for the others, close to the 12.6 MB/s floor of 1216x208 at 5000 fps.
// measureBestWriteRate takes a probe function, so scripted probes stand in for the disk:
//  - a healthy first sample is the only probe;
//  - a sample below 4x the floor, or one that ran into kMaxSeconds, is measured again, up to
//    kMaxProbes probes in all, and the best is kept (the bench's 15.2 then 469 gives 469);
//  - the repeats are bounded: by the probe count and by kMaxTotalSeconds of timed writing;
//  - a failed first probe is the answer (not repeated); a failed repeat keeps the best so far;
//  - writeRateFailsFloor is the storage.throughput Fail, and the real probe still works.

#include "backend/app/DestinationWriteRate.h"

#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using backend::app::DestinationWriteRate;
using backend::app::WriteRateMeasurement;
using backend::app::measureBestWriteRate;
using backend::app::writeRateFailsFloor;
using backend::app::writeRateIsSuspect;
namespace fs = std::filesystem;

namespace {

constexpr double kFloor = 12.6e6; // 1216x208 at 5000 fps, every 200th invalid frame

WriteRateMeasurement sample(double mbps, double seconds = 0.07)
{
    WriteRateMeasurement m;
    m.ok = true;
    m.bytesPerSecond = mbps * 1e6;
    m.seconds = seconds;
    m.bytes = static_cast<uint64_t>(m.bytesPerSecond * seconds);
    m.directory = "dir";
    m.note = "measured: sample";
    return m;
}

// A sample at an exact rate in bytes per second.
WriteRateMeasurement sampleBps(double bytesPerSecond, double seconds = 0.07)
{
    WriteRateMeasurement m = sample(1.0, seconds);
    m.bytesPerSecond = bytesPerSecond;
    return m;
}

WriteRateMeasurement failed(const std::string& why)
{
    WriteRateMeasurement m;
    m.directory = "dir";
    m.note = why;
    return m;
}

// Probes that return the scripted measurements in order (the last one repeats) and count calls.
struct Script {
    std::vector<WriteRateMeasurement> results;
    int calls{0};
    WriteRateMeasurement operator()()
    {
        const size_t i = std::min<size_t>(static_cast<size_t>(calls), results.size() - 1);
        ++calls;
        return results[i];
    }
};

WriteRateMeasurement run(Script& s, double floor)
{
    return measureBestWriteRate([&s] { return s(); }, floor);
}

} // namespace

int main()
{
    mib::test::Watchdog wd(60);
    wd.mark("healthy");

    // A healthy destination: one probe, the result untouched.
    {
        Script s{{sample(510.0)}};
        const auto m = run(s, kFloor);
        MIB_EXPECT(s.calls == 1 && m.probes == 1, "a healthy first sample is the only probe");
        MIB_EXPECT(m.ok && m.bytesPerSecond == 510e6 && m.note == "measured: sample", "result and note untouched");
        MIB_EXPECT(!writeRateIsSuspect(m, kFloor), "510 MB/s against a 12.6 MB/s floor is not suspect");
    }

    // The bench's case: 15.2 MB/s that ran into the 0.75 s bound, then a normal disk.
    wd.mark("bench case");
    {
        Script s{{sample(15.2, 0.80), sample(469.0)}};
        const auto m = run(s, kFloor);
        MIB_EXPECT(s.calls == 2 && m.probes == 2, "the slow sample is measured again");
        MIB_EXPECT(m.ok && m.bytesPerSecond == 469e6, "the better probe is kept");
        MIB_EXPECT(!writeRateFailsFloor(m, kFloor), "and it passes the floor");
        MIB_EXPECT(m.note.find("best of 2 probes (15.2, 469.0 MB/s)") != std::string::npos,
                   "the note lists the probes: " + m.note);
    }

    // The better probe need not be the last one.
    {
        Script s{{sample(20.0, 0.80), sample(30.0, 0.80), sample(25.0, 0.80)}};
        const auto m = run(s, kFloor);
        MIB_EXPECT(s.calls == 3, "three probes when none is trustworthy");
        MIB_EXPECT(m.bytesPerSecond == 30e6, "the best of the three, not the first or the last");
    }

    // A destination that stays slow: bounded at kMaxProbes, the best is reported and fails the floor.
    wd.mark("bounded");
    {
        Script s{{sample(10.0, 0.80), sample(9.0, 0.80), sample(11.0, 0.80), sample(500.0)}};
        const auto m = run(s, kFloor);
        MIB_EXPECT(s.calls == DestinationWriteRate::kMaxProbes && DestinationWriteRate::kMaxProbes == 3,
                   "never more than three probes");
        MIB_EXPECT(m.probes == 3 && m.bytesPerSecond == 11e6, "the best of the three");
        MIB_EXPECT(writeRateFailsFloor(m, kFloor), "11 MB/s fails the 12.6 MB/s floor");
    }

    // The 4x margin: exactly 4x the floor is trusted, just under it is measured again.
    wd.mark("margin");
    {
        Script atMargin{{sampleBps(4.0 * kFloor)}};
        run(atMargin, kFloor);
        MIB_EXPECT(atMargin.calls == 1, "4x the floor is trusted");
        Script under{{sampleBps(3.9 * kFloor), sampleBps(3.95 * kFloor), sampleBps(3.8 * kFloor)}};
        const auto m = run(under, kFloor);
        MIB_EXPECT(under.calls == 3, "under 4x the floor is measured again");
        MIB_EXPECT(!writeRateFailsFloor(m, kFloor), "and passes the floor (it is only close to it)");
    }

    // A fast sample that ran into the bound (a stall mid-probe) is measured again; without a
    // known floor only the bound decides.
    {
        Script s{{sample(100.0, 0.76), sample(400.0)}};
        run(s, kFloor);
        MIB_EXPECT(s.calls == 2, "a probe that ran into the 0.75 s bound is measured again");
        Script noFloor{{sample(1.0)}};
        run(noFloor, 0.0);
        MIB_EXPECT(noFloor.calls == 1, "without a floor, a probe inside the bound is trusted");
        Script noFloorBound{{sample(1.0, 0.80), sample(200.0)}};
        run(noFloorBound, 0.0);
        MIB_EXPECT(noFloorBound.calls == 2, "without a floor, a probe that ran into the bound is measured again");
    }

    // The time cap: no new probe once the probes spent kMaxTotalSeconds of timed writing.
    wd.mark("time cap");
    {
        Script s{{sample(15.0, 2.4), sample(500.0)}};
        const auto m = run(s, kFloor);
        MIB_EXPECT(s.calls == 1 && m.bytesPerSecond == 15e6,
                   "a first probe that already took 2.4 s is not followed by another");
        MIB_EXPECT(DestinationWriteRate::kMaxTotalSeconds == 3 * DestinationWriteRate::kMaxSeconds,
                   "the cap is kMaxProbes x kMaxSeconds");
    }

    // Failures.
    wd.mark("failures");
    {
        Script first{{failed("cannot create a probe file"), sample(500.0)}};
        const auto m = run(first, kFloor);
        MIB_EXPECT(first.calls == 1 && !m.ok && m.probes == 1 && m.note == "cannot create a probe file",
                   "a failed first probe is the answer and is not repeated");
        MIB_EXPECT(!writeRateFailsFloor(m, kFloor) && !writeRateIsSuspect(m, kFloor), "a failure is neither");

        Script repeat{{sample(15.0, 0.80), failed("writing the probe file failed"), sample(500.0)}};
        const auto kept = run(repeat, kFloor);
        MIB_EXPECT(repeat.calls == 2 && kept.ok && kept.bytesPerSecond == 15e6,
                   "a failed repeat keeps the best earlier result and stops");
    }

    // The floor test itself: the gate fails when the floor exceeds the rate; an unknown floor never fails.
    {
        MIB_EXPECT(writeRateFailsFloor(sample(10.0), kFloor) && !writeRateFailsFloor(sample(12.6), kFloor) &&
                       !writeRateFailsFloor(sample(10.0), 0.0),
                   "fails below the floor only, never without one");
    }

    // The real probe still works, and measureBestWriteRate over it takes between one and kMaxProbes.
    wd.mark("real probe");
    {
        mib::test::TempDir td("destination_write_rate");
        const std::string out = (td.path() / "run.h5").string();
        const auto one = backend::app::measureDestinationWriteRate(out, 98304);
        MIB_REQUIRE(one.ok, "probe: " + one.note);
        MIB_EXPECT(one.bytes > 0 && one.bytesPerSecond > 0 && one.probes == 1, "one probe measured something");
        const auto best = measureBestWriteRate(
            [&] { return backend::app::measureDestinationWriteRate(out, 98304); }, 1.0);
        MIB_EXPECT(best.ok && best.probes >= 1 && best.probes <= DestinationWriteRate::kMaxProbes,
                   "the real probe, repeated at most kMaxProbes times");
        size_t leftovers = 0;
        for (const auto& e : fs::directory_iterator(td.path())) {
            if (e.path().filename().string().rfind(".mib_write_rate_probe_", 0) == 0) ++leftovers;
        }
        MIB_EXPECT(leftovers == 0, "every probe removed its file");
    }

    if (mib::test::exitCode() == 0) std::printf("destination write rate verified\n");
    return mib::test::exitCode();
}
