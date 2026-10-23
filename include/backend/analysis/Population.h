#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Portable analysis composition. No acquisition, GUI, HDF5 or Python dependency.
// Mechanics values are supplied by the existing native LUT authority.
namespace backend::analysis {
enum class Truth : std::uint8_t { False, True, Unknown };
Truth logicalNot(Truth value);
Truth logicalAnd(Truth left, Truth right);
Truth logicalOr(Truth left, Truth right);

struct Interval {
    std::optional<double> minimum;
    std::optional<double> maximum;
};
// Inclusive, optional unbounded endpoints; invalid values stay Unknown.
// Invalid bounds throw before any event is evaluated (including empty input).
void validateInterval(const Interval& interval);
Truth evaluateInterval(std::optional<double> value, const Interval& interval);

struct RevisionKey {
    std::string snapshot;
    std::string sourceFingerprint;
    std::string plan;
    std::string application;
    std::string overrideRevision;
    std::string parameters;
    std::string qc;
    std::string mechanics;
    std::string engine;
    bool operator==(const RevisionKey& other) const;
};

struct MembershipRow {
    std::string key; // opaque exact source/set/row key, never a floating point ordinal
    Truth value;
};
struct Membership {
    RevisionKey revision;
    std::vector<MembershipRow> rows;
};

// Reject stale, duplicate, foreign or missing keys. Reorders output to input
// order; never joins by DataFrame/plot position. No partial result on failure.
Membership acceptWorkerMembership(const RevisionKey& expected,
                                  const std::vector<std::string>& inputKeys, Membership result);

enum class BooleanOperation { And, Or, Not };
// Three-valued membership restricted to the same parent AND QC universe.
// Right is required for And/Or and must be absent for Not. Inputs may be reordered.
Membership compose(const Membership& parent, const Membership& qc, const Membership& left,
                   const std::optional<Membership>& right, BooleanOperation operation);

struct Counts {
    std::uint64_t selected{0};
    std::uint64_t excluded{0};
    std::uint64_t unknown{0};
    std::uint64_t parentSelected{0};
    std::optional<double> percentParent;
};
Counts countMembership(const Membership& parent, const Membership& population);
} // namespace backend::analysis
