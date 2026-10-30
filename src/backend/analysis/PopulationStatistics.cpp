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
    const auto quantile = [&](long double p) {
        const long double position = (values.size() - 1) * p;
        const auto lowerIndex = static_cast<std::size_t>(position);
        const auto upperIndex = std::min(lowerIndex + 1, values.size() - 1);
        const long double fraction = position - lowerIndex;
        // Convex interpolation without overflowing an opposite-sign difference.
        return static_cast<double>((1 - fraction) * static_cast<long double>(values[lowerIndex]) +
                                   fraction * static_cast<long double>(values[upperIndex]));
    };
    result.p05 = quantile(.05L);
    result.p25 = quantile(.25L);
    result.p75 = quantile(.75L);
    result.p95 = quantile(.95L);
    const auto finite = [](long double v) -> std::optional<double> {
        const auto value = static_cast<double>(v);
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    };
    if (values.size() > 1) {
        long double squares = 0;
        if (scale != 0)
            for (const double value : values) {
                const long double delta = static_cast<long double>(value) / scale - normalized;
                squares += delta * delta;
            }
        const long double sd = std::sqrt(squares / (values.size() - 1));
        result.sampleStandardDeviation = finite(sd * scale);
        if (normalized != 0) result.cvPercent = finite(100 * sd / std::abs(normalized));
    }
    if (values.front() > 0) {
        long double logs = 0;
        for (const double value : values)
            logs += std::log(static_cast<long double>(value)) / values.size();
        result.geometricMean =
            finite(std::clamp(std::exp(logs), static_cast<long double>(values.front()),
                              static_cast<long double>(values.back())));
    }

    return result;
}
} // namespace backend::analysis
