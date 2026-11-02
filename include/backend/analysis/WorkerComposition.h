#pragma once
#include <cstdint>

namespace backend::analysis {
// Internal cxx seam, not the public Studio/Review or processing-core ABI.
std::uint8_t scalarPredicate(bool known, double value, bool hasMinimum, double minimum,
                             bool hasMaximum, double maximum);
std::uint8_t andPredicate(std::uint8_t left, std::uint8_t right);
} // namespace backend::analysis
