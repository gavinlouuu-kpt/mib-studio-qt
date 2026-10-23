#include "backend/analysis/WorkerComposition.h"
#include "backend/analysis/Population.h"

namespace backend::analysis {
std::uint8_t scalarPredicate(bool known, double value, bool hasMinimum, double minimum,
                             bool hasMaximum, double maximum) {
    return static_cast<std::uint8_t>(
        evaluateInterval(known ? std::optional<double>(value) : std::nullopt,
                         {hasMinimum ? std::optional<double>(minimum) : std::nullopt,
                          hasMaximum ? std::optional<double>(maximum) : std::nullopt}));
}
std::uint8_t andPredicate(std::uint8_t left, std::uint8_t right) {
    return static_cast<std::uint8_t>(
        logicalAnd(static_cast<Truth>(left), static_cast<Truth>(right)));
}
} // namespace backend::analysis
