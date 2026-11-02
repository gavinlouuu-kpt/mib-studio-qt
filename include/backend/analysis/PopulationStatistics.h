#pragma once
#include "backend/analysis/PopulationGraph.h"

namespace backend::analysis {
struct MeasurementRow {
    std::string key;
    std::optional<double> value;
};
struct MeasurementColumn {
    RevisionKey revision;
    std::string feature;
    std::string unit; // explicit, including "dimensionless"
    std::vector<MeasurementRow> rows;
};
struct PopulationStatistics {
    RevisionKey revision;
    std::string feature;
    std::string unit;
    std::uint64_t selected{0}, excluded{0}, unknownMembership{0};
    std::uint64_t measured{0}, missing{0}; // among selected events only
    std::optional<double> minimum, maximum, mean, median;
    std::optional<double> p05, p25, p75, p95;
    std::optional<double> sampleStandardDeviation, cvPercent, geometricMean;
};
// Exact finite raw values, not histogram estimates. Full keyed column coverage
// required, including excluded rows. Null/nonfinite values count as missing.
// Empty measured populations return null reductions. Keyed rows are bounded by
// kMaxKeyedEvaluationEvents; full recordings use columnStatistics
// (ColumnarEvaluation.h), which calls reduceMeasuredValues below.
// Quantiles use Hyndman-Fan type 7; sample SD divides by n-1 (null n<2).
// CV is 100*sampleSD/abs(mean), null at zero mean/nonrepresentable result.
// Geometric mean requires every measured value >0; missing remains counted.
// Does not assert completeness of a source snapshot or infer unobserved events.
PopulationStatistics populationStatistics(const Membership& population,
                                          const MeasurementColumn& column);
// The shared exact estimators over already-selected finite values (any order;
// sorted in place). Sets measured, minimum..geometricMean of `result`.
void reduceMeasuredValues(std::vector<double>& values, PopulationStatistics& result);
} // namespace backend::analysis
