#pragma once
#include <cstddef>
#include <string>

namespace xyo::jsnum::v8_decimal {
// Positive, finite, nonzero IEEE-754 binary64 -> ECMAScript shortest text.
// Writes at most 32 ASCII bytes for a positive finite nonzero double.
// Caller supplies at least 64 bytes; no heap allocations.
size_t toCharsPositive(double value, char* out);
std::string toStringPositive(double value);
} // namespace xyo::jsnum::v8_decimal
