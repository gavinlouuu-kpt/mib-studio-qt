// Pure Contract-v2 autofocus control math: an object-scoped focus-score
// (Laplacian variance) peak-seeker, kept entirely separate from the Contract-1
// ring-width setpoint controller in AutofocusMath.h. Qt-free and device-free so
// the maximize logic is unit tested without the CoreMOR hardware.
#pragma once

#include "backend/services/AutofocusMath.h" // clampVoltage

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

namespace backend::services::autofocus {

// One object's focus observation. Contract 2 autofocus consumes only these,
// never a manufactured frame-level value.
struct FocusSample {
    double laplacianVariance{std::numeric_limits<double>::quiet_NaN()};
    int64_t timestampNs{0};
    uint64_t frameIndex{0};
    int objectId{-1};
    int trackId{-1}; // >= 0 when tracking is available
};

// Validity policy: only finite detected-object samples are usable.
inline bool focusSampleValid(const FocusSample& s) {
    return std::isfinite(s.laplacianVariance);
}

// Stable identity for de-duplication: prefer the track id (a physical object
// across frames), else the per-frame object id.
inline int64_t focusSampleIdentity(const FocusSample& s) {
    return s.trackId >= 0 ? (static_cast<int64_t>(1) << 40) | static_cast<int64_t>(s.trackId)
                          : static_cast<int64_t>(s.objectId);
}

// Median focus score over valid samples, de-duplicated by (frameIndex,
// identity) so the same object/frame observation delivered twice is not
// double-counted. Returns NaN when no valid sample exists (never manufactures a
// value).
inline double medianFocusScore(const std::vector<FocusSample>& samples) {
    std::vector<double> values;
    values.reserve(samples.size());
    std::unordered_set<int64_t> seen;
    seen.reserve(samples.size() * 2);
    for (const FocusSample& s : samples) {
        if (!focusSampleValid(s)) {
            continue;
        }
        // Combine frame and identity into one key; collisions across the huge
        // 2^21 identity space are avoided by mixing with the frame index.
        const int64_t key =
            static_cast<int64_t>(s.frameIndex) * 2654435761LL ^ focusSampleIdentity(s);
        if (!seen.insert(key).second) {
            continue;
        }
        values.push_back(s.laplacianVariance);
    }
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return (n % 2 == 1) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

// A hill-climbing controller that maximizes the focus score. It probes a
// direction, keeps going while the score improves, reverses and refines to a
// fine step when the score degrades (having passed the peak), and holds when
// the change is within a tolerance. Every commanded voltage is clamped.
class FocusScoreController {
public:
    struct Params {
        double coarseStep{1.0};
        double fineStep{0.2};
        double minVoltage{0.0};
        double maxVoltage{100.0};
        // A |score change| at or below this is treated as "no real change" (hold
        // near the peak / ignore noise).
        double holdTolerance{1e-6};
    };

    FocusScoreController() = default;
    explicit FocusScoreController(const Params& p) : p_(p) {}

    void setParams(const Params& p) { p_ = p; }
    void reset() {
        havePrevious_ = false;
        fine_ = false;
        improvedThisDirection_ = false;
        direction_ = +1;
        bestScore_ = -std::numeric_limits<double>::infinity();
    }

    // Given the fresh median score measured at `currentVoltage` (after the last
    // step settled), return the next voltage to command. Holds by returning
    // `currentVoltage` (clamped) unchanged.
    double step(double currentVoltage, double freshScore) {
        const double v = clamp(currentVoltage);

        if (!havePrevious_) {
            havePrevious_ = true;
            previousScore_ = freshScore;
            bestScore_ = freshScore;
            bestVoltage_ = v;
            direction_ = +1;
            fine_ = false;
            improvedThisDirection_ = false;
            return command(v, direction_); // probe a direction
        }

        if (freshScore > bestScore_) {
            bestScore_ = freshScore;
            bestVoltage_ = v;
        }

        const double delta = freshScore - previousScore_;
        previousScore_ = freshScore;

        if (delta > p_.holdTolerance) {
            // Still improving: keep the current direction and step size.
            improvedThisDirection_ = true;
            return command(v, direction_);
        }
        if (delta < -p_.holdTolerance) {
            // Degraded: reverse. If we had been improving in this direction we
            // overshot the peak, so refine to the fine step; otherwise the
            // initial probe simply went the wrong way and we stay coarse.
            direction_ = -direction_;
            if (improvedThisDirection_) {
                fine_ = true;
            }
            improvedThisDirection_ = false;
            return command(v, direction_);
        }
        // Within tolerance: refine once, then hold.
        if (!fine_) {
            fine_ = true;
            return command(v, direction_);
        }
        return v; // hold
    }

    double bestVoltage() const { return bestVoltage_; }
    double bestScore() const { return bestScore_; }
    bool isFine() const { return fine_; }
    int direction() const { return direction_; }

private:
    double stepSize() const { return fine_ ? p_.fineStep : p_.coarseStep; }
    double clamp(double v) const { return clampVoltage(v, p_.minVoltage, p_.maxVoltage); }
    double command(double v, int dir) const {
        return clamp(v + static_cast<double>(dir) * stepSize());
    }

    Params p_{};
    bool havePrevious_{false};
    bool fine_{false};
    bool improvedThisDirection_{false};
    int direction_{+1};
    double previousScore_{0.0};
    double bestScore_{-std::numeric_limits<double>::infinity()};
    double bestVoltage_{0.0};
};

} // namespace backend::services::autofocus
