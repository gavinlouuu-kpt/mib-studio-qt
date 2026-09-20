// AI Experiment Supervisor — deterministic rule provider (issue #422, phase B).
//
// A transparent, network-free DecisionProvider that maps snapshot metrics to
// contract answers with fixed thresholds. It is the CI baseline every other
// provider (JEV, future models) is compared against in the evaluation
// harness, and the provider AppBackend installs when no remote provider is
// configured. Its "distributions" are synthetic margins (one-hot softened by
// how far the metric sits from the threshold) so calibration code paths are
// exercised deterministically; they are not probabilities of correctness.
#pragma once

#include "backend/supervisor/DecisionProvider.h"

namespace backend::supervisor {

struct RuleThresholds {
    double lowContrast{20.0};          // brightness Q3-Q1 below this -> LOW_CONTRAST
    double saturationBrightness{250.0}; // mean Q4 at/above -> OVEREXPOSURE
    double underexposureMedian{25.0};   // mean Q2 at/below -> UNDEREXPOSURE
    double excessRejectionFraction{0.75};
    double excessFalseDetectionObjectsPerSecond{400.0}; // implausible object rate
    double debrisAreaFractionOfMin{0.5}; // mean area below 0.5*minArea -> small debris
    double marginalFrameLossFraction{0.02};
    double triggerFailureFraction{0.10};
    double noObjectsAfterSeconds{10.0};
    double highConcentrationObjectsPerSecond{150.0};
};

class RuleProvider final : public DecisionProvider {
public:
    explicit RuleProvider(RuleThresholds thresholds = {});

    std::string name() const override { return "rule"; }
    std::string version() const override { return "rule/1"; }

    DecisionResult evaluate(const ExperimentSnapshot& snapshot,
                            const DecisionPolicy& policy) override;

    const RuleThresholds& thresholds() const { return thresholds_; }

private:
    RuleThresholds thresholds_;
};

} // namespace backend::supervisor
