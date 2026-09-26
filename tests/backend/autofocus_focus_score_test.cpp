// V2-4 proof: the Contract-2 focus-score peak-seeker maximizes an object-level
// Laplacian-variance signal. It converges near the peak from both sides,
// holds on a plateau / under sub-tolerance noise without runaway, respects the
// voltage clamps, and consumes only de-duplicated finite object samples. The
// Contract-1 ring-width setpoint controller (AutofocusMath.h) is untouched.
#include "backend/services/AutofocusFocusScore.h"
#include "support/assert.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace af = backend::services::autofocus;

namespace {

af::FocusScoreController::Params params() {
    af::FocusScoreController::Params p;
    p.coarseStep = 1.0;
    p.fineStep = 0.2;
    p.minVoltage = 0.0;
    p.maxVoltage = 100.0;
    p.holdTolerance = 0.1;
    return p;
}

// Run the controller against a score curve f(v); return the settled voltage.
template <class F>
double run(af::FocusScoreController& c, double startVoltage, F&& f, int steps) {
    double v = startVoltage;
    for (int i = 0; i < steps; ++i) {
        v = c.step(v, f(v));
    }
    return v;
}

void testConvergesFromBothSides() {
    const auto peak = [](double v) { return -(v - 60.0) * (v - 60.0); };

    af::FocusScoreController below(params());
    const double vBelow = run(below, 30.0, peak, 400);
    MIB_EXPECT(std::abs(below.bestVoltage() - 60.0) <= 0.5,
               "converges near the peak from below");
    MIB_EXPECT(std::abs(vBelow - 60.0) <= 0.5, "settles near the peak from below");

    af::FocusScoreController above(params());
    const double vAbove = run(above, 90.0, peak, 400);
    MIB_EXPECT(std::abs(above.bestVoltage() - 60.0) <= 0.5,
               "converges near the peak from above");
    MIB_EXPECT(std::abs(vAbove - 60.0) <= 0.5, "settles near the peak from above");
    MIB_EXPECT(above.isFine(), "refines to the fine step near the peak");
}

void testPlateauHoldsNoRunaway() {
    af::FocusScoreController c(params());
    const double start = 50.0;
    const double v = run(c, start, [](double) { return 5.0; }, 200);
    // A flat curve must not drive the voltage away: at most one coarse + one
    // fine step before holding.
    MIB_EXPECT(std::abs(v - start) <= 1.0 + 0.2 + 1e-9, "plateau holds without runaway");
}

void testSubToleranceNoiseHolds() {
    af::FocusScoreController c(params());
    const double start = 50.0;
    int k = 0;
    // Deterministic noise with amplitude below holdTolerance.
    const double v = run(c, start, [&](double) { return 5.0 + 0.04 * ((k++ % 3) - 1); }, 200);
    MIB_EXPECT(std::abs(v - start) <= 1.0 + 0.2 + 1e-9, "sub-tolerance noise does not oscillate");
}

void testClampToBound() {
    af::FocusScoreController c(params());
    // Monotonic increasing score: the peak is above maxVoltage.
    const double v = run(c, 95.0, [](double x) { return x; }, 100);
    MIB_EXPECT(std::abs(v - 100.0) <= 1e-9, "drives to the upper bound");
    MIB_EXPECT(std::abs(c.bestVoltage() - 100.0) <= 1e-9, "best voltage clamped to the bound");
}

void testSampleValidityAndDedup() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    MIB_EXPECT(!af::focusSampleValid({nan, 0, 0, 1, -1}), "NaN sample is invalid");
    MIB_EXPECT(af::focusSampleValid({12.5, 0, 0, 1, -1}), "finite sample is valid");

    // Median ignores NaN and de-duplicates by (frame, identity).
    std::vector<af::FocusSample> samples = {
        {10.0, 0, 5, /*obj*/ 1, /*track*/ -1},
        {10.0, 0, 5, 1, -1},  // exact duplicate of the above -> dropped
        {20.0, 0, 5, 2, -1},  // different object, same frame
        {nan, 0, 5, 3, -1},   // invalid -> dropped
        {30.0, 0, 6, 1, -1},  // same object, next frame
    };
    // Deduped valid values: {10, 20, 30} -> median 20.
    MIB_EXPECT(std::abs(af::medianFocusScore(samples) - 20.0) <= 1e-9,
               "median over de-duplicated finite samples");

    MIB_EXPECT(std::isnan(af::medianFocusScore({{nan, 0, 0, 1, -1}})),
               "no valid sample -> NaN (never manufactured)");

    // Track identity de-dups across frames differently from object id.
    std::vector<af::FocusSample> tracked = {
        {5.0, 0, 1, 9, /*track*/ 7},
        {5.0, 0, 1, 9, 7}, // duplicate track+frame -> dropped
    };
    MIB_EXPECT(std::abs(af::medianFocusScore(tracked) - 5.0) <= 1e-9, "track dedup keeps one");
}

} // namespace

int main() {
    testConvergesFromBothSides();
    testPlateauHoldsNoRunaway();
    testSubToleranceNoiseHolds();
    testClampToBound();
    testSampleValidityAndDedup();
    return mib::test::exitCode();
}
