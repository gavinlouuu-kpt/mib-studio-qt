#pragma once
#include "backend/analysis/Population.h"

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
};
// Exact finite raw values, not histogram estimates. Full keyed column coverage
// required, including excluded rows. Null/nonfinite values count as missing.
// Empty measured populations return null reductions. Maximum 10,000 rows.
// Does not assert completeness of a source snapshot or infer unobserved events.
PopulationStatistics populationStatistics(const Membership& population,
                                          const MeasurementColumn& column);
} // namespace backend::analysis
