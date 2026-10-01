// Invariant coverage for the dot-grid design registry: parsing and its
// rejection rules (unique ids and seeds, slugs, status, geometry, version),
// merge semantics, and multi-design decoding — every frame is attributed to
// the design it was rendered from (with chip and pose), a frame of an
// unregistered design never decodes, and designs with a different dot
// geometry decode through their own detection pass. With a path argument it
// also checks the bundled registry (resources/defaults/dot_grid/registry.json)
// and that its Wafer_soRT entry regenerates the archived codebook.
#include "backend/processing/DotGridCodebook.h"
#include "backend/processing/DotGridDecoder.h"
#include "backend/processing/DotGridRegistry.h"
#include "support/assert.h"
#include "support/opencv_tsan.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace backend::dotgrid;

namespace {

std::string entry(const std::string& id, int seed, double pitch = 30.0, double dot = 12.0,
                  double shift = 5.0, const std::string& extra = "") {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"({"id": "%s", "name": "Design %s", "revision": "r1", "seed": %d,
                      "columns": 1200, "rows": 1200, "pitch_um": %g, "dot_diameter_um": %g,
                      "displacement_um": %g, "origin_um": [0.0, 0.0]%s})",
                  id.c_str(), id.c_str(), seed, pitch, dot, shift, extra.c_str());
    return buf;
}

std::string doc(const std::string& entries, int version = 1) {
    return "{\"version\": " + std::to_string(version) + ", \"designs\": [" + entries + "]}";
}

bool rejects(const std::string& text, const std::string& expectFragment) {
    Registry r;
    std::string err;
    const bool ok = Registry::parse(text, r, &err);
    MIB_EXPECT(!ok, "rejected: " + expectFragment);
    MIB_EXPECT(err.find(expectFragment) != std::string::npos,
               "error mentions '" + expectFragment + "': " + err);
    return !ok;
}

} // namespace

int main(int argc, char** argv) {
    mib::test::serializeOpenCvUnderTsan();
    const std::string chips =
        R"(, "chips": [{"name": "R0C0", "x_min_um": 5000, "y_min_um": 5000, "x_max_um": 15000, "y_max_um": 15000},
                       {"name": "R0C1", "x_min_um": 20000, "y_min_um": 5000, "x_max_um": 30000, "y_max_um": 15000}])";

    // 1. Parse: three designs, two sharing a geometry, one with its own.
    Registry reg;
    std::string err;
    MIB_REQUIRE(Registry::parse(doc(entry("alpha", 11, 30, 12, 5, chips) + "," +
                                    entry("beta", 12, 30, 12, 5, chips) + "," +
                                    entry("gamma.v2", 13, 40, 16, 7, chips)),
                                reg, &err),
                "valid registry parses: " + err);
    MIB_EXPECT(reg.size() == 3, "three designs");
    MIB_REQUIRE(reg.find("beta") != nullptr, "find by id");
    MIB_EXPECT(reg.find("beta")->name == "Design beta" && reg.find("beta")->revision == "r1" &&
                   reg.find("beta")->status == "active",
               "metadata and default status");
    MIB_EXPECT(reg.find("beta")->codebook->params().seed == 12, "seed carried into the codebook");
    MIB_EXPECT(reg.find("beta")->codebook->designName() == "beta", "codebook named after the id");
    MIB_EXPECT(reg.find("beta")->codebook->chips().size() == 2, "chip table");
    MIB_EXPECT(reg.find("delta") == nullptr, "unknown id");
    {
        Registry again;
        MIB_REQUIRE(Registry::parse(doc(entry("alpha", 11, 30, 12, 5, chips) + "," +
                                        entry("beta", 12, 30, 12, 5, chips) + "," +
                                        entry("gamma.v2", 13, 40, 16, 7, chips)),
                                    again),
                    "reparse");
        MIB_EXPECT(again.fingerprint() == reg.fingerprint(), "fingerprint is stable");
    }

    // 2. Rejections: the whole document is refused.
    rejects(doc(entry("alpha", 11) + "," + entry("alpha", 12)), "duplicate design id");
    rejects(doc(entry("alpha", 11) + "," + entry("beta", 11)), "seed 11 used by both");
    rejects(doc(entry("Alpha", 11)), "lowercase slug");
    rejects(doc(entry("-alpha", 11)), "lowercase slug");
    rejects(doc(entry("alpha", 11, 30, 12, 5, R"(, "status": "draft")")), "status");
    rejects(doc(entry("alpha", 11, 30, 22, 5)), "dots would touch");
    rejects(doc(entry("alpha", 11), 2), "version");
    rejects("{\"version\": 1, \"designs\": [", "parse error");
    {
        Registry r;
        MIB_EXPECT(!Registry::loadFile("/nonexistent/registry.json", r, &err) &&
                       err.find("cannot open") != std::string::npos,
                   "missing file reported");
    }

    // 3. Merge: identical entries collapse, clashes are skipped with a warning.
    {
        Registry merged = reg;
        Registry local;
        MIB_REQUIRE(Registry::parse(doc(entry("alpha", 11, 30, 12, 5, chips) + "," + // identical
                                        entry("beta", 99) + "," +                     // id clash
                                        entry("delta", 13) + "," +                    // seed clash
                                        entry("epsilon", 14)),                        // new
                                    local, &err),
                    "local registry parses: " + err);
        std::vector<std::string> warnings;
        merged.merge(local, &warnings);
        MIB_EXPECT(merged.size() == 4, "only the new design was added");
        MIB_EXPECT(merged.find("epsilon") != nullptr, "new design present");
        MIB_EXPECT(merged.find("beta")->codebook->params().seed == 12, "bundled entry wins");
        MIB_EXPECT(warnings.size() == 2, "one warning per clash");
        MIB_EXPECT(merged.fingerprint() != reg.fingerprint(), "fingerprint follows contents");
    }

    // 4. Multi-design decode: each frame is attributed to its own design.
    auto shared = std::make_shared<const Registry>(reg);
    Decoder decoder(shared);
    DecoderConfig cfg;
    cfg.umPerPxHint = 0.31;
    const struct {
        const char* id;
        double x, y, theta;
        bool mirrored;
        const char* chip;
    } cases[] = {{"alpha", 10000.0, 9000.0, 17.0, true, "R0C0"},
                 {"beta", 25000.0, 11000.0, -64.0, false, "R0C1"},
                 {"gamma.v2", 12000.0, 7000.0, 133.0, true, "R0C0"},
                 {"beta", 30000.0 + 3000.0, 20000.0, 5.0, true, ""}};
    double multiMs = 0.0;
    for (const auto& c : cases) {
        ViewPose pose;
        pose.centreXUm = c.x;
        pose.centreYUm = c.y;
        pose.thetaDeg = c.theta;
        pose.umPerPx = 0.293;
        pose.mirrored = c.mirrored;
        const cv::Mat img = renderView(*reg.find(c.id)->codebook, pose, RenderOptions{});
        const DecodeResult r = decoder.decode(img, cfg);
        const std::string what = std::string(c.id) + " at (" + std::to_string(c.x) + "," +
                                 std::to_string(c.y) + "): reason='" + r.reason + "' design='" +
                                 r.designId + "'";
        MIB_EXPECT(r.ok, "decodes: " + what);
        MIB_EXPECT(r.designId == c.id, "attributed to its design: " + what);
        MIB_EXPECT(r.designName == std::string("Design ") + c.id, "design name: " + what);
        MIB_EXPECT(r.chip == c.chip, "chip '" + std::string(c.chip) + "': got '" + r.chip + "'");
        MIB_EXPECT(r.designsTried == 3, "every design tried");
        MIB_EXPECT(std::abs(r.centreXUm - c.x) < 1.0 && std::abs(r.centreYUm - c.y) < 1.0,
                   "position: " + what);
        MIB_EXPECT(r.mirrored == c.mirrored, "mirror flag: " + what);
        multiMs += r.decodeMs;
    }

    // 5. A design that is not registered never decodes (no false attribution).
    {
        CodebookParams p;
        p.seed = 777;
        p.columns = 1200;
        p.rows = 1200;
        const Codebook stranger = Codebook::generate(p);
        int falseHits = 0;
        for (int k = 0; k < 6; ++k) {
            ViewPose pose;
            pose.centreXUm = 6000.0 + 3100.0 * k;
            pose.centreYUm = 25000.0 - 2700.0 * k;
            pose.thetaDeg = -150.0 + 55.0 * k;
            pose.umPerPx = 0.293;
            pose.mirrored = (k % 2) == 0;
            RenderOptions opt;
            opt.seed = static_cast<uint32_t>(k);
            const DecodeResult r = decoder.decode(renderView(stranger, pose, opt), cfg);
            if (r.ok) ++falseHits;
            MIB_EXPECT(!r.ok && r.designId.empty(),
                       "unregistered design rejected (view " + std::to_string(k) +
                           "): reason='" + r.reason + "' design='" + r.designId + "'");
        }
        MIB_EXPECT(falseHits == 0, "no false attributions");
    }

    // 6. Clean failures and the single-codebook constructor.
    {
        const cv::Mat blank(1200, 1920, CV_8UC1, cv::Scalar(180));
        const DecodeResult r = decoder.decode(blank, cfg);
        MIB_EXPECT(!r.ok && r.reason == "too few dots", "blank frame: " + r.reason);
        const DecodeResult none =
            Decoder(std::make_shared<const Registry>()).decode(blank, cfg);
        MIB_EXPECT(!none.ok && none.reason == "no codebook", "empty registry: " + none.reason);
        Decoder single(reg.find("alpha")->codebook);
        ViewPose pose;
        pose.centreXUm = 10000.0;
        pose.centreYUm = 9000.0;
        const DecodeResult s =
            single.decode(renderView(*reg.find("alpha")->codebook, pose, RenderOptions{}), cfg);
        MIB_EXPECT(s.ok && s.designId.empty() && s.designsTried == 1,
                   "single-codebook mode has no design id: " + s.reason);
        std::printf("decode: %.1f ms mean over 3 designs (2 geometries), %.1f ms for one design\n",
                    multiMs / 4.0, s.decodeMs);
    }

    // 7. The bundled registry parses and regenerates the archived Wafer_soRT codebook.
    if (argc > 1) {
        const std::string registryPath = argv[1];
        Registry bundled;
        MIB_REQUIRE(Registry::loadFile(registryPath, bundled, &err),
                    "bundled registry loads: " + err);
        MIB_REQUIRE(bundled.find("wafer-sort-rt") != nullptr, "Wafer_soRT registered");
        const auto& cb = *bundled.find("wafer-sort-rt")->codebook;
        MIB_EXPECT(cb.params().seed == 7, "Wafer_soRT keeps seed 7");
        const std::string dir = registryPath.substr(0, registryPath.find_last_of("/\\") + 1);
        Codebook archived;
        MIB_REQUIRE(
            Codebook::loadJson(dir + "wafer_soRT_2025-03-16_seed7_p30.json", archived, &err),
            "archived codebook loads: " + err);
        MIB_EXPECT(archived.phi() == cb.phi() && archived.psi() == cb.psi(),
                   "registry entry regenerates the archived codebook");
        MIB_EXPECT(archived.chips().size() == cb.chips().size(), "same chip table");
    }

    return mib::test::exitCode();
}
