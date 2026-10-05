// Gold conformance of the bundled dot-grid codec core (ADR 0010).
//
// Reads scripts/dot_grid/gold/codec-contract<N>.json (path in argv[1]) and
// requires the core serving that contract to reproduce it:
//  - encode: m-sequence, SHA-256 of the full phi/psi arrays, heads, last
//    values and probe bits for every reference codebook - exactly;
//  - decode: every "decode" case decodes within tolerance of the rendered
//    truth, every "reject" case does not decode.
// The same file is checked against the Python reference core by
// `dotgrid_cli.py gold` (CTest scripts.dot_grid_reference), so the mask
// generator and the app cannot drift apart. The reference changes only in a
// PR labelled gold-reference-change.
#include "backend/processing/DotGridCodec.h"
#include "backend/processing/ProcessingCoreLoader.h"
#include "support/assert.h"
#include "support/opencv_tsan.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>

using namespace backend::dotgrid;

namespace {

std::string phasesDigest(const std::vector<int>& phases) {
    std::string text;
    for (size_t k = 0; k < phases.size(); ++k) {
        if (k) text += ',';
        text += std::to_string(phases[k]);
    }
    return backend::processing::processingCoreBytesSha256(reinterpret_cast<const uint8_t*>(text.data()),
                                                          text.size());
}

} // namespace

int main(int argc, char** argv) {
    mib::test::serializeOpenCvUnderTsan();
    MIB_REQUIRE(argc > 1, "usage: dot_grid_codec_gold_test <codec-contract<N>.json>");
    std::ifstream in(argv[1]);
    MIB_REQUIRE(static_cast<bool>(in), std::string("cannot open ") + argv[1]);
    const nlohmann::json gold = nlohmann::json::parse(in);
    MIB_REQUIRE(gold.at("gold_schema_version").get<int>() == 1, "gold schema version 1");

    const int contract = gold.at("codec_contract").get<int>();
    const CodecSet codecs = CodecSet::bundled();
    const ICodec* codec = codecs.find(contract);
    MIB_REQUIRE(codec != nullptr, "a core serves codec contract " + std::to_string(contract));
    const CodecIdentity& id = codec->identity();
    MIB_EXPECT(id.line == gold.at("line").get<std::string>(), "contract line name matches the gold reference");
    std::printf("core %s (contract %d '%s', %s)\n", id.coreVersion.c_str(), id.contract, id.line.c_str(),
                id.source.c_str());

    // ---- encode: exact
    const auto& enc = gold.at("encode");
    for (const auto& c : enc.at("codebooks")) {
        CodebookParams p;
        p.seed = c.at("seed").get<uint64_t>();
        p.columns = c.at("columns").get<int>();
        p.rows = c.at("rows").get<int>();
        p.pitchUm = enc.at("pitch_um").get<double>();
        p.dotDiameterUm = enc.at("dot_diameter_um").get<double>();
        p.displacementUm = enc.at("displacement_um").get<double>();
        const Codebook cb = codec->encode(p, {}, "gold");
        const std::string tag =
            "seed " + std::to_string(p.seed) + " " + std::to_string(p.columns) + "x" + std::to_string(p.rows);
        std::string mns;
        for (int b : cb.mns())
            mns += static_cast<char>('0' + b);
        MIB_EXPECT(mns == enc.at("mns").get<std::string>(), tag + ": m-sequence");
        MIB_EXPECT(phasesDigest(cb.phi()) == c.at("phi_sha256").get<std::string>(), tag + ": phi digest");
        MIB_EXPECT(phasesDigest(cb.psi()) == c.at("psi_sha256").get<std::string>(), tag + ": psi digest");
        const auto phiHead = c.at("phi_head").get<std::vector<int>>();
        const auto psiHead = c.at("psi_head").get<std::vector<int>>();
        MIB_EXPECT(std::equal(phiHead.begin(), phiHead.end(), cb.phi().begin()), tag + ": phi head");
        MIB_EXPECT(std::equal(psiHead.begin(), psiHead.end(), cb.psi().begin()), tag + ": psi head");
        MIB_EXPECT(cb.phi().back() == c.at("phi_last").get<int>() && cb.psi().back() == c.at("psi_last").get<int>(),
                   tag + ": last phases");
        for (const auto& pr : c.at("probes")) {
            const auto bits = cb.bits(pr.at("i").get<int>(), pr.at("j").get<int>());
            const auto want = pr.at("bits").get<std::vector<int>>();
            MIB_EXPECT(bits.first == want[0] && bits.second == want[1],
                       tag + ": bits at (" + std::to_string(pr.at("i").get<int>()) + "," +
                           std::to_string(pr.at("j").get<int>()) + ")");
        }
    }

    // ---- decode: behavioural, against the rendered truth
    const auto& dec = gold.at("decode");
    const auto& dc = dec.at("codebook");
    std::map<uint64_t, std::shared_ptr<const Codebook>> patterns;
    auto pattern = [&](uint64_t seed) {
        auto& slot = patterns[seed];
        if (!slot) {
            CodebookParams p;
            p.seed = seed;
            p.columns = dc.at("columns").get<int>();
            p.rows = dc.at("rows").get<int>();
            p.pitchUm = dc.at("pitch_um").get<double>();
            p.dotDiameterUm = dc.at("dot_diameter_um").get<double>();
            p.displacementUm = dc.at("displacement_um").get<double>();
            slot = std::make_shared<const Codebook>(codec->encode(p, {}, "gold-" + std::to_string(seed)));
        }
        return slot;
    };
    const auto goldPattern = pattern(dc.at("seed").get<uint64_t>());
    DesignDecoder decoder(codecs, std::make_shared<const Registry>(Registry::single(goldPattern, "gold")));
    int met = 0, total = 0;
    for (const auto& c : dec.at("cases")) {
        ++total;
        const std::string name = c.at("name").get<std::string>();
        const auto& jp = c.at("pose");
        ViewPose pose;
        pose.centreXUm = jp.at("centre_um").at(0).get<double>();
        pose.centreYUm = jp.at("centre_um").at(1).get<double>();
        pose.thetaDeg = jp.at("theta_deg").get<double>();
        pose.umPerPx = jp.at("um_per_px").get<double>();
        pose.mirrored = jp.at("mirrored").get<bool>();
        pose.width = jp.at("width").get<int>();
        pose.height = jp.at("height").get<int>();
        cv::Mat img;
        if (c.value("blank", false)) {
            img = cv::Mat(pose.height, pose.width, CV_8UC1, cv::Scalar(180));
        } else {
            RenderOptions opt;
            opt.seed = c.value("noise_seed", 0u);
            opt.missingFraction = c.value("missing_fraction", 0.0);
            if (c.contains("channel")) {
                const auto& ch = c.at("channel");
                opt.channel = true;
                opt.channelX0 = ch.at("from_um").at(0).get<double>();
                opt.channelY0 = ch.at("from_um").at(1).get<double>();
                opt.channelX1 = ch.at("to_um").at(0).get<double>();
                opt.channelY1 = ch.at("to_um").at(1).get<double>();
                opt.channelWidthUm = ch.at("width_um").get<double>();
                opt.bandUm = ch.at("band_um").get<double>();
            }
            img = renderView(*pattern(c.value("pattern_seed", dc.at("seed").get<uint64_t>())), pose, opt);
        }
        DecoderConfig cfg;
        cfg.umPerPxHint = pose.umPerPx * dec.at("um_per_px_hint_factor").get<double>();
        const DecodeResult r = decoder.decode(img, cfg);
        bool ok;
        std::string why;
        if (c.value("expect", std::string("decode")) == "reject") {
            ok = !r.ok;
            why = "must not decode";
        } else {
            const auto& tol = c.at("tolerance");
            const double err = std::hypot(r.centreXUm - pose.centreXUm, r.centreYUm - pose.centreYUm);
            const double dtheta = std::abs(std::fmod(r.thetaDeg - pose.thetaDeg + 540.0, 360.0) - 180.0);
            ok = r.ok && err <= tol.at("position_um").get<double>() &&
                 dtheta <= tol.at("theta_deg").get<double>() &&
                 std::abs(r.umPerPx - pose.umPerPx) <= tol.at("um_per_px_rel").get<double>() * pose.umPerPx &&
                 r.mirrored == pose.mirrored && r.codecContract == contract;
            why = "reason='" + r.reason + "' err=" + std::to_string(err) + "um dtheta=" + std::to_string(dtheta);
        }
        MIB_EXPECT(ok, "gold decode case " + name + ": " + why);
        met += ok ? 1 : 0;
        std::printf("%-24s %s (%.0f ms)\n", name.c_str(), ok ? "met" : "NOT MET", r.decodeMs);
    }
    std::printf("decode cases met: %d/%d\n", met, total);
    return mib::test::exitCode();
}
