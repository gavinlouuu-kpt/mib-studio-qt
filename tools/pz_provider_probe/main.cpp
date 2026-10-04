// pz_provider_probe: run PzDevMemExecutionProvider on the PZ7035 PS for N
// seconds and report what reached its sink, as `pzres monitor` does for the
// raw ring. Checks the C++ reader against the board without the application.
//   pz_provider_probe SECONDS [RUN_ID] [--configure]
// --configure first commits, through the provider, the profile the profile
// compiler builds from the host defaults and the board's synthetic E-modulus
// grid; processing.pz_profile_compiler shows these equal the page.bin and
// lut.bin that pzres config wrote on 2026-10-03.
// Stop pzres first: the ring TAIL register is shared, one reader at a time.
// Build: tools/pz_provider_probe/build.sh (Yocto SDK, armv7).
#include "backend/processing/pz/PzExecutionProviders.h"
#include "backend/processing/pz/PzProfileCompiler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

namespace {
backend::processing::pz::CompiledProfile boardDefaultsProfile(backend::EModulusLut& lut) {
    std::vector<double> g(200 * 200);
    for (int d = 0; d < 200; ++d)
        for (int a = 0; a < 200; ++a)
            g[static_cast<size_t>(d * 200 + a)] = (d >= 190 && a >= 190)
                                                      ? std::numeric_limits<double>::quiet_NaN()
                                                      : 0.5 + 0.03 * a + 25.0 * std::pow(d / 200.0, 1.5);
    lut.loadGrid(20.0, 1.5, 200, 0.0, 0.0025, 200, std::move(g));
    backend::processing::pz::UnetCellsProfileInputs in;
    auto& c = in.config;
    c.enable_area_range_check = true;
    c.area_threshold_min = 60;
    c.area_threshold_max = 290;
    c.enable_deformability_range_check = true;
    c.deformability_threshold_max = 0.5;
    c.enable_target_group = true;
    c.target_group_area_min = 72;
    c.target_group_area_max = 191;
    c.target_group_deformability_max = 0.3;
    c.enable_target_group_emodulus = true;
    c.target_group_emodulus_max = 10.0;
    in.pixelToMicron = 0.5;
    in.lut = &lut;
    return backend::processing::pz::compileUnetCellsV2(in);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pz_provider_probe SECONDS [RUN_ID]\n");
        return 2;
    }
    const double seconds = std::atof(argv[1]);
    const uint64_t runId = argc > 2 && argv[2][0] != '-' ? std::strtoull(argv[2], nullptr, 0) : 1;
    backend::processing::pz::PzDevMemExecutionProvider provider({});
    std::atomic<uint64_t> frames{0}, cells{0}, valid{0}, cut{0}, empty{0}, invalid{0}, idGaps{0}, last{0};
    std::atomic<uint64_t> maxLatencyTicks{0};
    provider.setSink([&](backend::processing::ProviderFrame&& f) {
        const uint64_t prev = last.exchange(f.frameId);
        if (prev && f.frameId != prev + 1) ++idGaps;
        ++frames;
        cells += f.cells.size();
        empty += f.empty() ? 1 : 0;
        invalid += f.invalid() ? 1 : 0;
        for (const auto& c : f.cells) {
            valid += c.valid() ? 1 : 0;
            cut += c.cutOff ? 1 : 0;
        }
    });
    std::string error;
    bool configure = false;
    for (int i = 1; i < argc; ++i) configure = configure || std::strcmp(argv[i], "--configure") == 0;
    if (configure) {
        backend::EModulusLut lut;
        const auto profile = boardDefaultsProfile(lut);
        if (!profile.ok() || !provider.configure(profile, &error)) {
            std::fprintf(stderr, "configure failed: %s\n", profile.ok() ? error.c_str() : profile.errors.front().c_str());
            return 1;
        }
        std::printf("configured: epoch %u, page word 2 = %u, table %zu B\n", provider.status().epoch,
                    profile.page[2], profile.table0.size());
    }
    if (!provider.start(runId, &error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    double next = 1.0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t >= next || t >= seconds) {
            const auto st = provider.status();
            std::printf("%6.1f s: %llu frames (%.0f/s), %llu cells (%llu valid, %llu cut off), %llu empty, "
                        "%llu invalid; decode errors %llu, sequence gaps %llu, frame-id gaps %llu, overruns %llu, "
                        "incomplete %llu\n",
                        t, (unsigned long long)frames.load(), frames.load() / t, (unsigned long long)cells.load(),
                        (unsigned long long)valid.load(), (unsigned long long)cut.load(),
                        (unsigned long long)empty.load(), (unsigned long long)invalid.load(),
                        (unsigned long long)st.decodeErrors, (unsigned long long)st.sequenceGaps,
                        (unsigned long long)idGaps.load(), (unsigned long long)st.overruns,
                        (unsigned long long)st.incompleteFrames);
            std::fflush(stdout);
            next += 1.0;
        }
        if (t >= seconds) break;
    }
    provider.stop();
    const auto st = provider.status();
    const bool clean = st.decodeErrors == 0 && st.sequenceGaps == 0 && idGaps.load() == 0 && st.overruns == 0;
    std::printf("%s: %llu frames, %llu cells\n", clean ? "PASS" : "FAIL", (unsigned long long)frames.load(),
                (unsigned long long)cells.load());
    return clean ? 0 : 1;
}
