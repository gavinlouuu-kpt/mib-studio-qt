// Invariant + golden coverage for the dot-grid decoder on synthetic 20x/10x
// views: rotation, mirror (glass-side viewing), channel keep-out band, noise,
// random dropouts, and clean failure on images without a pattern.
#include "backend/processing/DotGridCodebook.h"
#include "backend/processing/DotGridDecoder.h"
#include "support/assert.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace backend::dotgrid;

namespace {

double angleDiff(double a, double b) {
    double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
    return std::abs(d);
}

std::string describe(const DecodeResult& r) {
    return "ok=" + std::to_string(r.ok) + " reason='" + r.reason +
           "' dots=" + std::to_string(r.dots) + " votes=" + std::to_string(r.votes) +
           " agreement=" + std::to_string(r.agreement) + " centre=(" + std::to_string(r.centreXUm) +
           "," + std::to_string(r.centreYUm) + ")" + " theta=" + std::to_string(r.thetaDeg) +
           " scale=" + std::to_string(r.umPerPx);
}

bool checkPose(const DecodeResult& r, const ViewPose& pose, double tolUm, const std::string& what) {
    const bool ok = r.ok && std::abs(r.centreXUm - pose.centreXUm) < tolUm &&
                    std::abs(r.centreYUm - pose.centreYUm) < tolUm && r.mirrored == pose.mirrored &&
                    angleDiff(r.thetaDeg, pose.thetaDeg) < 0.3 &&
                    std::abs(r.umPerPx - pose.umPerPx) < 0.003;
    MIB_EXPECT(ok, what + ": " + describe(r));
    return ok;
}

} // namespace

int main() {
    CodebookParams p;
    p.seed = 7;
    p.columns = 3700;
    p.rows = 3700;
    p.pitchUm = 30.0;
    p.dotDiameterUm = 12.0;
    p.displacementUm = 5.0;
    auto cb = std::make_shared<const Codebook>(
        Codebook::generate(p, {{"R3C2", 43015.8, 55964.3, 66984.3, 62779.0}}));
    Decoder decoder(cb);
    DecoderConfig cfg;
    cfg.umPerPxHint = 0.32; // deliberately 10% off: the decoder must measure the true scale

    // 1. Clean 20x views at assorted rotations, both handednesses, sub-micron accuracy.
    const double thetas[] = {0.0, 37.5, -120.0, 91.0, 179.0, -45.0};
    for (int k = 0; k < 6; ++k) {
        ViewPose pose;
        pose.centreXUm = 40000.0 + 1234.5 * k;
        pose.centreYUm = 52000.0 - 987.25 * k;
        pose.thetaDeg = thetas[k];
        pose.umPerPx = 0.293;
        pose.mirrored = (k % 2) == 1;
        RenderOptions opt;
        opt.seed = static_cast<uint32_t>(k);
        const cv::Mat img = renderView(*cb, pose, opt);
        const DecodeResult r = decoder.decode(img, cfg);
        checkPose(r, pose, 1.0, "clean 20x view " + std::to_string(k));
        MIB_EXPECT(r.residualPx < 3.0, "lattice residual small: " + describe(r));
        std::printf("clean 20x view %d: %d dots, decode %.1f ms\n", k, r.dots, r.decodeMs);
    }

    // 2. Chip identification from the codebook chip table.
    {
        ViewPose pose;
        pose.centreXUm = 50000.0;
        pose.centreYUm = 60000.0;
        pose.umPerPx = 0.293;
        RenderOptions opt;
        const DecodeResult r = decoder.decode(renderView(*cb, pose, opt), cfg);
        MIB_EXPECT(r.ok && r.chip == "R3C2", "chip id: " + describe(r));
        MIB_EXPECT(std::abs(r.pixelToWafer[0] * 960.0 + r.pixelToWafer[1] * 600.0 +
                            r.pixelToWafer[2] - 50000.0) < 1.0,
                   "pixelToWafer maps the image centre");
    }

    // 3. Channel through the field of view with a 50 um keep-out band (the
    //    mid-channel 20x use case), along and across the image, mirrored.
    for (int k = 0; k < 4; ++k) {
        ViewPose pose;
        pose.centreXUm = 30000.0 + 700.0 * k;
        pose.centreYUm = 30000.0 + 300.0 * k;
        pose.thetaDeg = (k < 2 ? 2.0 : 88.0) + 0.7 * k;
        pose.umPerPx = 0.293;
        pose.mirrored = (k % 2) == 0;
        RenderOptions opt;
        opt.seed = 100 + static_cast<uint32_t>(k);
        opt.channel = true;
        const double t = pose.thetaDeg * CV_PI / 180.0;
        const double dx = std::cos(t), dy = std::sin(t);
        const double ox = -dy * 40.0, oy = dx * 40.0; // channel 40 um off the image centre line
        opt.channelX0 = pose.centreXUm + ox - dx * 3000.0;
        opt.channelY0 = pose.centreYUm + oy - dy * 3000.0;
        opt.channelX1 = pose.centreXUm + ox + dx * 3000.0;
        opt.channelY1 = pose.centreYUm + oy + dy * 3000.0;
        opt.bandUm = 15.0 + 50.0 + p.dotDiameterUm / 2;
        const DecodeResult r = decoder.decode(renderView(*cb, pose, opt), cfg);
        checkPose(r, pose, 1.0, "channel band view " + std::to_string(k));
    }

    // 4. Heavy noise and 10% random dropouts still decode at 10x.
    {
        ViewPose pose;
        pose.centreXUm = 61000.0;
        pose.centreYUm = 22000.0;
        pose.thetaDeg = -33.0;
        pose.umPerPx = 0.586;
        RenderOptions opt;
        opt.noiseSigma = 20.0;
        opt.missingFraction = 0.10;
        opt.seed = 9;
        DecoderConfig cfg10 = cfg;
        cfg10.umPerPxHint = 0.6;
        const DecodeResult r = decoder.decode(renderView(*cb, pose, opt), cfg10);
        checkPose(r, pose, 1.0, "noisy 10x view with dropouts");
    }

    // 5. No pattern: fails cleanly with a reason, never a bogus pose.
    {
        const cv::Mat blank(1200, 1920, CV_8UC1, cv::Scalar(180));
        const DecodeResult r = decoder.decode(blank, cfg);
        MIB_EXPECT(!r.ok && r.reason == "too few dots", "blank image: " + describe(r));
        cv::Mat noise(1200, 1920, CV_8UC1);
        cv::randu(noise, 0, 255);
        const DecodeResult rn = decoder.decode(noise, cfg);
        MIB_EXPECT(!rn.ok, "pure noise never decodes: " + describe(rn));
        const DecodeResult re = decoder.decode(cv::Mat(), cfg);
        MIB_EXPECT(!re.ok && re.reason == "empty image", "empty image: " + describe(re));
    }

    // 6. 16-bit input is accepted (normalised internally).
    {
        ViewPose pose;
        pose.centreXUm = 45000.0;
        pose.centreYUm = 45000.0;
        pose.umPerPx = 0.293;
        RenderOptions opt;
        cv::Mat img16;
        renderView(*cb, pose, opt).convertTo(img16, CV_16U, 16.0);
        const DecodeResult r = decoder.decode(img16, cfg);
        checkPose(r, pose, 1.0, "16-bit view");
    }

    return mib::test::exitCode();
}
