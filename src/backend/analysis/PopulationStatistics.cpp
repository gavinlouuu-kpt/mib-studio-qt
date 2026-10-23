#include "backend/analysis/PopulationStatistics.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace backend::analysis {
PopulationStatistics populationStatistics(const Membership& population,
                                          const MeasurementColumn& column) {
    if (population.rows.size() > 10000 || column.rows.size() > 10000)
        throw std::invalid_argument("Statistics event budget exceeded");
    if (column.feature.empty() || column.unit.empty() || !(population.revision == column.revision))
        throw std::invalid_argument("Unqualified measurement column");
    std::vector<std::string> keys;
    std::unordered_map<std::string, std::optional<double>> measurements;
    for (const auto& row : column.rows) {
        keys.push_back(row.key);
        if (!measurements.emplace(row.key, row.value).second)
            throw std::invalid_argument("Duplicate measurement key");
    }
    const auto accepted = acceptWorkerMembership(column.revision, keys, population);
    PopulationStatistics result;
    result.revision = column.revision;
    result.feature = column.feature;
    result.unit = column.unit;
    std::vector<double> values;
    for (const auto& row : accepted.rows) {
        if (row.value == Truth::Unknown) {
            ++result.unknownMembership;
            continue;
        }
        if (row.value == Truth::False) {
            ++result.excluded;
            continue;
        }
        ++result.selected;
        const auto value = measurements.at(row.key);
        if (!value || !std::isfinite(*value)) {
            ++result.missing;
            continue;
        }
        values.push_back(*value);
    }
    result.measured = values.size();
    if (values.empty()) return result;
    std::sort(values.begin(), values.end());
    result.minimum = values.front();
    result.maximum = values.back();
    const auto middle = values.size() / 2;
    // Same-sign difference preserves tiny equal values without overflowing;
    // opposite-sign addition cannot overflow.
    const double upper = values[middle];
    const double lower = values[(values.size() - 1) / 2];
    result.median = std::signbit(lower) == std::signbit(upper) ? lower + (upper - lower) / 2.0
                                                               : (lower + upper) / 2.0;
    // Scale before summing: finite extreme values must not overflow a double sum.
    const double scale = std::max(std::abs(values.front()), std::abs(values.back()));
    long double sum = 0, correction = 0;
    if (scale != 0) {
        for (double value : values) {
            const long double term = static_cast<long double>(value) / scale - correction;
            const long double next = sum + term;
            correction = (next - sum) - term;
            sum = next;
        }
    }
    const auto normalized = std::clamp(sum / values.size(), -1.0L, 1.0L);
    result.mean = static_cast<double>(normalized * scale);
    return result;
}
} // namespace backend::analysis
