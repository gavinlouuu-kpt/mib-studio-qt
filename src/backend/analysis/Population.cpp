#include "backend/analysis/Population.h"

#include <cmath>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace backend::analysis {
namespace {
void validateTruth(Truth value) {
    if (value != Truth::False && value != Truth::True && value != Truth::Unknown)
        throw std::invalid_argument("Invalid membership truth value");
}
std::vector<std::string> keys(const Membership& membership) {
    std::vector<std::string> result;
    result.reserve(membership.rows.size());
    for (const auto& row : membership.rows)
        result.push_back(row.key);
    return result;
}
void validateRevision(const RevisionKey& key) {
    for (const auto* field :
         {&key.snapshot, &key.sourceFingerprint, &key.plan, &key.application, &key.overrideRevision,
          &key.parameters, &key.qc, &key.mechanics, &key.engine})
        if (field->empty()) throw std::invalid_argument("Unpinned analysis revision");
}
} // namespace

Truth logicalNot(Truth value) {
    validateTruth(value);
    return value == Truth::Unknown ? Truth::Unknown
           : value == Truth::True  ? Truth::False
                                   : Truth::True;
}
Truth logicalAnd(Truth left, Truth right) {
    validateTruth(left);
    validateTruth(right);
    if (left == Truth::False || right == Truth::False) return Truth::False;
    if (left == Truth::Unknown || right == Truth::Unknown) return Truth::Unknown;
    return Truth::True;
}
Truth logicalOr(Truth left, Truth right) {
    validateTruth(left);
    validateTruth(right);
    if (left == Truth::True || right == Truth::True) return Truth::True;
    if (left == Truth::Unknown || right == Truth::Unknown) return Truth::Unknown;
    return Truth::False;
}
void validateInterval(const Interval& interval) {
    if ((interval.minimum && !std::isfinite(*interval.minimum)) ||
        (interval.maximum && !std::isfinite(*interval.maximum)) ||
        (interval.minimum && interval.maximum && *interval.minimum > *interval.maximum))
        throw std::invalid_argument("Nonfinite or reversed scalar interval");
}
Truth evaluateInterval(std::optional<double> value, const Interval& interval) {
    validateInterval(interval);
    if (!value || !std::isfinite(*value)) return Truth::Unknown;
    return ((!interval.minimum || *value >= *interval.minimum) &&
            (!interval.maximum || *value <= *interval.maximum))
               ? Truth::True
               : Truth::False;
}
bool RevisionKey::operator==(const RevisionKey& other) const {
    return std::tie(snapshot, sourceFingerprint, plan, application, overrideRevision, parameters,
                    qc, mechanics, engine) == std::tie(other.snapshot, other.sourceFingerprint,
                                                       other.plan, other.application,
                                                       other.overrideRevision, other.parameters,
                                                       other.qc, other.mechanics, other.engine);
}
Membership acceptWorkerMembership(const RevisionKey& expected,
                                  const std::vector<std::string>& inputKeys, Membership result) {
    validateRevision(expected);
    if (!(expected == result.revision)) throw std::invalid_argument("Stale analysis result");
    if (inputKeys.size() != result.rows.size())
        throw std::invalid_argument("Incomplete worker key coverage");
    std::unordered_set<std::string> inputs;
    for (const auto& key : inputKeys)
        if (key.empty() || !inputs.insert(key).second)
            throw std::invalid_argument("Empty or duplicate source key");
    std::unordered_map<std::string, Truth> output;
    for (const auto& row : result.rows) {
        validateTruth(row.value);
        if (!inputs.count(row.key) || !output.emplace(row.key, row.value).second)
            throw std::invalid_argument("Foreign or duplicate worker key");
    }
    Membership accepted{expected, {}};
    accepted.rows.reserve(inputKeys.size());
    for (const auto& key : inputKeys)
        accepted.rows.push_back({key, output.at(key)});
    return accepted;
}
Membership compose(const Membership& parent, const Membership& qc, const Membership& left,
                   const std::optional<Membership>& right, BooleanOperation operation) {
    if ((operation == BooleanOperation::Not) != !right.has_value())
        throw std::invalid_argument("Boolean operand arity mismatch");
    if (operation != BooleanOperation::And && operation != BooleanOperation::Or &&
        operation != BooleanOperation::Not)
        throw std::invalid_argument("Unknown Boolean operation");
    const auto inputKeys = keys(parent);
    const auto p = acceptWorkerMembership(parent.revision, inputKeys, parent);
    const auto q = acceptWorkerMembership(parent.revision, inputKeys, qc);
    const auto l = acceptWorkerMembership(parent.revision, inputKeys, left);
    const auto r =
        right ? acceptWorkerMembership(parent.revision, inputKeys, *right) : Membership{};
    Membership result{parent.revision, {}};
    result.rows.reserve(inputKeys.size());
    for (std::size_t i = 0; i < inputKeys.size(); ++i) {
        const auto predicate = operation == BooleanOperation::Not ? logicalNot(l.rows[i].value)
                               : operation == BooleanOperation::And
                                   ? logicalAnd(l.rows[i].value, r.rows[i].value)
                                   : logicalOr(l.rows[i].value, r.rows[i].value);
        result.rows.push_back(
            {inputKeys[i], logicalAnd(logicalAnd(p.rows[i].value, q.rows[i].value), predicate)});
    }
    return result;
}
Counts countMembership(const Membership& parent, const Membership& population) {
    const auto inputKeys = keys(parent);
    const auto p = acceptWorkerMembership(parent.revision, inputKeys, parent);
    const auto c = acceptWorkerMembership(parent.revision, inputKeys, population);
    Counts result;
    for (std::size_t i = 0; i < inputKeys.size(); ++i) {
        if (c.rows[i].value == Truth::True && p.rows[i].value != Truth::True)
            throw std::invalid_argument("Population admits event outside known parent");
        result.parentSelected += p.rows[i].value == Truth::True;
        result.selected += c.rows[i].value == Truth::True;
        result.excluded += c.rows[i].value == Truth::False;
        result.unknown += c.rows[i].value == Truth::Unknown;
    }
    if (result.parentSelected)
        result.percentParent = 100.0 * static_cast<double>(result.selected) /
                               static_cast<double>(result.parentSelected);
    return result;
}
} // namespace backend::analysis
