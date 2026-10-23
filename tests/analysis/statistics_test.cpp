#include "backend/analysis/PopulationStatistics.h"
#include "support/assert.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
using namespace backend::analysis;
template <class F> bool refused(F action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
int main() {
    RevisionKey revision{"s", "fp", "p", "a", "o", "params", "qc", "lut", "engine"};
    Membership population{revision,
                          {{"epoch1/9007199254740993", Truth::True},
                           {"epoch1/9007199254740994", Truth::True},
                           {"missing", Truth::True},
                           {"excluded", Truth::False},
                           {"unknown", Truth::Unknown}}};
    MeasurementColumn column{revision,
                             "area",
                             "um2",
                             {{"unknown", 1000},
                              {"excluded", 999},
                              {"missing", {}},
                              {"epoch1/9007199254740994", 8},
                              {"epoch1/9007199254740993", 2}}};
    auto result = populationStatistics(population, column);
    MIB_EXPECT(result.selected == 3 && result.measured == 2 && result.missing == 1 &&
                   result.excluded == 1 && result.unknownMembership == 1,
               "conserved missing/membership accounting");
    MIB_EXPECT(result.mean == 5 && result.median == 5 && result.minimum == 2 && result.maximum == 8,
               "raw keyed reductions exclude nonselected events");
    std::reverse(column.rows.begin(), column.rows.end());
    MIB_EXPECT(populationStatistics(population, column).mean == 5, "order independent");
    auto bad = column;
    bad.revision.mechanics = "stale";
    MIB_EXPECT(refused([&] { populationStatistics(population, bad); }), "stale refusal");
    bad = column;
    bad.rows[0].key = "foreign";
    MIB_EXPECT(refused([&] { populationStatistics(population, bad); }), "foreign refusal");
    bad = column;
    bad.rows[0].key = bad.rows[1].key;
    MIB_EXPECT(refused([&] { populationStatistics(population, bad); }), "duplicate refusal");
    bad = column;
    bad.rows.pop_back();
    MIB_EXPECT(refused([&] { populationStatistics(population, bad); }),
               "incomplete coverage refusal");
    bad = column;
    bad.unit.clear();
    MIB_EXPECT(refused([&] { populationStatistics(population, bad); }), "unit required");
    population.rows = {{"a", Truth::True}, {"b", Truth::True}};
    column.rows = {{"a", std::numeric_limits<double>::max()},
                   {"b", std::numeric_limits<double>::max()}};
    result = populationStatistics(population, column);
    MIB_EXPECT(result.mean == std::numeric_limits<double>::max() && result.median == result.mean,
               "finite extreme values do not overflow");
    column.rows = {{"a", std::numeric_limits<double>::denorm_min()},
                   {"b", std::numeric_limits<double>::denorm_min()}};
    MIB_EXPECT(populationStatistics(population, column).median ==
                   std::numeric_limits<double>::denorm_min(),
               "equal subnormal median preserved");
    column.rows = {{"a", -std::numeric_limits<double>::max()},
                   {"b", std::numeric_limits<double>::max()}};
    result = populationStatistics(population, column);
    MIB_EXPECT(result.mean == 0 && result.median == 0, "opposite extremes");
    column.rows = {{"a", std::numeric_limits<double>::infinity()},
                   {"b", std::numeric_limits<double>::quiet_NaN()}};
    result = populationStatistics(population, column);
    MIB_EXPECT(result.missing == 2 && !result.mean && !result.median && !result.minimum &&
                   !result.maximum,
               "nonfinite values explicit missing");
    population.rows.clear();
    column.rows.clear();
    result = populationStatistics(population, column);
    MIB_EXPECT(result.selected == 0 && !result.mean && !result.median, "empty null reductions");
    population.rows = {{"a", Truth::True}};
    column.rows = {{"a", -3}};
    MIB_EXPECT(populationStatistics(population, column).median == -3, "singleton median");
    column.rows.resize(10001);
    MIB_EXPECT(refused([&] { populationStatistics(population, column); }),
               "budget refusal before allocation");
    return mib::test::exitCode();
}
