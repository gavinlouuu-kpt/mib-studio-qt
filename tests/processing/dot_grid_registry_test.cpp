// Invariant coverage for the dot-grid design registry: parsing and its
// rejection rules (unique ids and seeds, slugs, status, geometry, version),
// merge semantics, and multi-design decoding — every frame is attributed to
// the design it was rendered from (with chip and pose), a frame of an
// unregistered design never decodes, and designs with a different dot
// geometry decode through their own detection pass. With a path argument it
// also checks the bundled registry (resources/defaults/dot_grid/registry.json)
// and that its Wafer_soRT entry regenerates the archived codebook.
// Codec contracts (ADR 0010): codec_contract is required; a design whose
// contract no active core serves is listed as unsupported and never decoded;
// one core per contract; every result names the core that produced it.
#include "backend/processing/DotGridCodebook.h"
#include "backend/processing/DotGridCodec.h"
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

// A core that returns a canned result, to test DesignDecoder's combination
// rules independently of real decoding.
class CannedCodec final : public ICodec {
public:
    CannedCodec(int contract, DecodeResult result) : result_(std::move(result)) {
        id_.contract = contract;
        id_.coreVersion = "test";
        id_.source = "plugin";
        id_.line = "canned";
    }
    const CodecIdentity& identity() const override { return id_; }
    Codebook encode(const CodebookParams& p, std::vector<Chip> chips, std::string name) const override {
        return Codebook::generate(p, std::move(chips), std::move(name));
    }
    DecodeResult decode(const std::shared_ptr<const Registry>& designs, const cv::Mat&,
                        const DecoderConfig&) const override {
        DecodeResult r = result_;
        r.designsTried = static_cast<int>(designs->size());
        return r;
    }

private:
    CodecIdentity id_;
    DecodeResult result_;
};

DecodeResult canned(bool ok, int stage, const std::string& reason, const std::string& design = {},
                    int votes = 0) {
    DecodeResult r;
    r.ok = ok;
    r.stage = stage;
    r.reason = reason;
    r.designId = design;
    r.votes = votes;
    return r;
}

std::string entry(const std::string& id, int seed, double pitch = 30.0, double dot = 12.0,
                  double shift = 5.0, const std::string& extra = "", int contract = 1) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  R"({"id": "%s", "name": "Design %s", "revision": "r1", "codec_contract": %d,
                      "seed": %d, "columns": 1200, "rows": 1200, "pitch_um": %g,
                      "dot_diameter_um": %g, "displacement_um": %g, "origin_um": [0.0, 0.0]%s})",
                  id.c_str(), id.c_str(), contract, seed, pitch, dot, shift, extra.c_str());
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
    rejects(doc(R"({"id": "alpha", "seed": 11, "columns": 1200, "rows": 1200, "pitch_um": 30,
                    "dot_diameter_um": 12, "displacement_um": 5})"),
            "codec_contract (a positive integer) is required");
    rejects(doc(entry("alpha", 11, 30, 12, 5, "", 0)), "codec_contract");
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

    // 6b. Codec contracts and cores (ADR 0010).
    {
        const auto bundled = bundledCodec();
        const CodecIdentity& id = bundled->identity();
        MIB_EXPECT(id.contract == 1 && id.line == "mseq63-delta2" && id.source == "bundled" &&
                       id.coreVersion == MIB_DOTGRID_CORE_VERSION && !id.runtimeFingerprint.empty(),
                   "bundled core identity: " + id.buildId);
        MIB_EXPECT(codecLineName(1) == "mseq63-delta2" && codecLineName(99).empty(), "line names");
        CodecSet set = CodecSet::bundled();
        MIB_EXPECT(!set.add(bundledCodec(), &err) && err.find("already active") != std::string::npos,
                   "one core per contract: " + err);
        MIB_EXPECT(set.find(1) != nullptr && set.find(2) == nullptr, "find by contract");

        // A design of a contract without a core: listed, never decoded, still unique.
        Registry mixed;
        MIB_REQUIRE(Registry::parse(doc(entry("alpha", 11, 30, 12, 5, chips) + "," +
                                        entry("future", 21, 30, 12, 5, "", 2)),
                                    mixed, &err),
                    "registry with a contract-2 design parses: " + err);
        MIB_EXPECT(mixed.size() == 1 && mixed.find("future") == nullptr, "contract-2 design not decodable");
        MIB_REQUIRE(mixed.unsupported().size() == 1, "contract-2 design listed as unsupported");
        MIB_EXPECT(mixed.unsupported()[0].id == "future" && mixed.unsupported()[0].codecContract == 2 &&
                       mixed.unsupported()[0].seed == 21,
                   "unsupported entry keeps id, contract, seed");
        rejects(doc(entry("alpha", 11) + "," + entry("future", 11, 30, 12, 5, "", 2)),
                "seed 11 used by both");
        MIB_EXPECT(mixed.fingerprint().find("future:c2:21") != std::string::npos,
                   "fingerprint covers unsupported designs");
        {
            Registry merged;
            merged.merge(mixed);
            merged.merge(mixed); // identical again: no duplicates, no warnings
            MIB_EXPECT(merged.size() == 1 && merged.unsupported().size() == 1, "merge keeps unsupported once");
        }

        // DesignDecoder stamps the core identity; a contract-2 frame never decodes.
        DesignDecoder dd(CodecSet::bundled(), std::make_shared<const Registry>(mixed));
        ViewPose pose;
        pose.centreXUm = 10000.0;
        pose.centreYUm = 9000.0;
        pose.mirrored = true;
        const DecodeResult r = dd.decode(renderView(*mixed.find("alpha")->codebook, pose, RenderOptions{}), cfg);
        MIB_EXPECT(r.ok && r.designId == "alpha" && r.codecContract == 1 &&
                       r.coreVersion == MIB_DOTGRID_CORE_VERSION && r.coreSource == "bundled",
                   "result names its core: contract " + std::to_string(r.codecContract) + " " +
                       r.coreVersion + " " + r.coreSource + " " + r.reason);
        CodebookParams futureParams;
        futureParams.seed = 21;
        futureParams.columns = 1200;
        futureParams.rows = 1200;
        const Codebook futurePattern = Codebook::generate(futureParams);
        const DecodeResult f = dd.decode(renderView(futurePattern, pose, RenderOptions{}), cfg);
        MIB_EXPECT(!f.ok && f.designId.empty(), "frame of the unsupported design does not decode: " + f.reason);

        Registry onlyFuture;
        MIB_REQUIRE(Registry::parse(doc(entry("future", 21, 30, 12, 5, "", 2)), onlyFuture, &err), err);
        const DecodeResult none = DesignDecoder(CodecSet::bundled(), std::make_shared<const Registry>(onlyFuture))
                                      .decode(renderView(futurePattern, pose, RenderOptions{}), cfg);
        MIB_EXPECT(!none.ok && none.reason.find("no active core") != std::string::npos,
                   "only unsupported designs: names the missing core: " + none.reason);

        // The contract-1 algorithm refuses a hand-built design that claims another contract.
        Registry forged;
        Design fake;
        fake.id = "forged";
        fake.codecContract = 2;
        fake.codebook = std::make_shared<const Codebook>(futurePattern);
        MIB_REQUIRE(forged.add(fake, &err), err);
        const DecodeResult g = Decoder(std::make_shared<const Registry>(forged))
                                   .decode(renderView(futurePattern, pose, RenderOptions{}), cfg);
        MIB_EXPECT(!g.ok && g.reason == "no design of codec contract 1",
                   "contract-1 decoder fails closed on another contract: " + g.reason);
    }

    // 6c. Review fixes: the fingerprint covers what results show; ambiguity and
    //     failure ranking survive combining several cores.
    {
        Registry base, renamed, moved;
        const std::string chipA = R"(, "chips": [{"name": "R0C0", "x_min_um": 5000, "y_min_um": 5000, "x_max_um": 15000, "y_max_um": 15000}])";
        const std::string chipB = R"(, "chips": [{"name": "R0C0", "x_min_um": 5000, "y_min_um": 5000, "x_max_um": 15500, "y_max_um": 15000}])";
        MIB_REQUIRE(Registry::parse(doc(entry("alpha", 11, 30, 12, 5, chipA)), base, &err), err);
        std::string renamedEntry = entry("alpha", 11, 30, 12, 5, chipA);
        renamedEntry.replace(renamedEntry.find("Design alpha"), 12, "Alpha v2");
        MIB_REQUIRE(Registry::parse(doc(renamedEntry), renamed, &err), err);
        MIB_REQUIRE(Registry::parse(doc(entry("alpha", 11, 30, 12, 5, chipB)), moved, &err), err);
        MIB_EXPECT(base.fingerprint() != renamed.fingerprint(), "a renamed design changes the fingerprint");
        MIB_EXPECT(base.fingerprint() != moved.fingerprint(), "a corrected chip outline changes the fingerprint");
        Registry merged = base;
        std::vector<std::string> warnings;
        merged.merge(moved, &warnings);
        MIB_EXPECT(warnings.size() == 1, "an edited local copy is reported, not silently dropped");

        // Contract 1 finds two designs, contract 2 finds a third: still ambiguous.
        Registry mixedContracts;
        Design a, b, c;
        CodebookParams p;
        p.columns = 200;
        p.rows = 200;
        p.seed = 31;
        a.id = "a";
        a.codecContract = 1;
        a.codebook = std::make_shared<const Codebook>(Codebook::generate(p));
        p.seed = 32;
        b = a;
        b.id = "b";
        b.codebook = std::make_shared<const Codebook>(Codebook::generate(p));
        p.seed = 33;
        c = a;
        c.id = "c";
        c.codecContract = 2;
        c.codebook = std::make_shared<const Codebook>(Codebook::generate(p));
        MIB_REQUIRE(mixedContracts.add(a, &err) && mixedContracts.add(b, &err) && mixedContracts.add(c, &err), err);
        auto shared = std::make_shared<const Registry>(mixedContracts);
        const cv::Mat frame(10, 10, CV_8UC1, cv::Scalar(0));
        {
            CodecSet set;
            set.add(std::make_shared<CannedCodec>(1, canned(false, kDecodeStageAmbiguousDesign, "ambiguous design (a, b)")));
            set.add(std::make_shared<CannedCodec>(2, canned(true, kDecodeStageOk, "", "c")));
            const DecodeResult r = DesignDecoder(set, shared).decode(frame, DecoderConfig{});
            MIB_EXPECT(!r.ok && r.reason == "ambiguous design (a, b, c)" && r.designId.empty(),
                       "inner ambiguity is never masked by a hit in another core: " + r.reason);
        }
        {
            // Failures: the attempt that got furthest wins over one with more votes.
            CodecSet set;
            set.add(std::make_shared<CannedCodec>(1, canned(false, 2, "no consistent code window", "", 9)));
            set.add(std::make_shared<CannedCodec>(2, canned(false, 4, "bit agreement too low", "", 3)));
            const DecodeResult r = DesignDecoder(set, shared).decode(frame, DecoderConfig{});
            MIB_EXPECT(!r.ok && r.reason == "bit agreement too low" && r.codecContract == 2,
                       "failure ranked by stage, then votes: " + r.reason);
        }
        {
            CodecSet set;
            set.add(std::make_shared<CannedCodec>(1, canned(false, 0, "too few dots")));
            set.add(std::make_shared<CannedCodec>(2, canned(true, kDecodeStageOk, "", "c")));
            const DecodeResult r = DesignDecoder(set, shared).decode(frame, DecoderConfig{});
            MIB_EXPECT(r.ok && r.designId == "c" && r.coreSource == "plugin" && r.designsTried == 3,
                       "one hit across cores decodes: " + r.reason);
        }
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
        MIB_EXPECT(bundled.find("wafer-sort-rt")->codecContract == 1, "Wafer_soRT is codec contract 1");
        MIB_EXPECT(bundled.unsupported().empty(), "every bundled design has a core in this build");
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
