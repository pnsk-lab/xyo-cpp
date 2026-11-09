// Copyright 2011 the V8 project authors. All rights reserved.
// Derived from V8 src/numbers/conversions.cc, SignificandToChars helpers.
// Licensed under the V8 BSD-style license; see
// third_party/dragonbox/LICENSE-V8 in the repository.
//
// Adaptations: self-contained C++20 namespace, std::memcpy, no DCHECK
// dependencies; uses Dragonbox as vendored from jk-jeon/dragonbox.
//
#include "v8_decimal.hpp"

#include "../../third_party/dragonbox/dragonbox.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace xyo::jsnum::v8_decimal {
namespace {
static constexpr char kRadix100Table[200] = {
    '0', '0', '0', '1', '0', '2', '0', '3', '0', '4',  //
    '0', '5', '0', '6', '0', '7', '0', '8', '0', '9',  //
    '1', '0', '1', '1', '1', '2', '1', '3', '1', '4',  //
    '1', '5', '1', '6', '1', '7', '1', '8', '1', '9',  //
    '2', '0', '2', '1', '2', '2', '2', '3', '2', '4',  //
    '2', '5', '2', '6', '2', '7', '2', '8', '2', '9',  //
    '3', '0', '3', '1', '3', '2', '3', '3', '3', '4',  //
    '3', '5', '3', '6', '3', '7', '3', '8', '3', '9',  //
    '4', '0', '4', '1', '4', '2', '4', '3', '4', '4',  //
    '4', '5', '4', '6', '4', '7', '4', '8', '4', '9',  //
    '5', '0', '5', '1', '5', '2', '5', '3', '5', '4',  //
    '5', '5', '5', '6', '5', '7', '5', '8', '5', '9',  //
    '6', '0', '6', '1', '6', '2', '6', '3', '6', '4',  //
    '6', '5', '6', '6', '6', '7', '6', '8', '6', '9',  //
    '7', '0', '7', '1', '7', '2', '7', '3', '7', '4',  //
    '7', '5', '7', '6', '7', '7', '7', '8', '7', '9',  //
    '8', '0', '8', '1', '8', '2', '8', '3', '8', '4',  //
    '8', '5', '8', '6', '8', '7', '8', '8', '8', '9',  //
    '9', '0', '9', '1', '9', '2', '9', '3', '9', '4',  //
    '9', '5', '9', '6', '9', '7', '9', '8', '9', '9'   //
};

static constexpr char kRadix100HeadTable[200] = {
    '\0', '\0', '1', '\0', '2', '\0', '3', '\0', '4', '\0',  //
    '5',  '\0', '6', '\0', '7', '\0', '8', '\0', '9', '\0',  //
    '1',  '0',  '1', '1',  '1', '2',  '1', '3',  '1', '4',   //
    '1',  '5',  '1', '6',  '1', '7',  '1', '8',  '1', '9',   //
    '2',  '0',  '2', '1',  '2', '2',  '2', '3',  '2', '4',   //
    '2',  '5',  '2', '6',  '2', '7',  '2', '8',  '2', '9',   //
    '3',  '0',  '3', '1',  '3', '2',  '3', '3',  '3', '4',   //
    '3',  '5',  '3', '6',  '3', '7',  '3', '8',  '3', '9',   //
    '4',  '0',  '4', '1',  '4', '2',  '4', '3',  '4', '4',   //
    '4',  '5',  '4', '6',  '4', '7',  '4', '8',  '4', '9',   //
    '5',  '0',  '5', '1',  '5', '2',  '5', '3',  '5', '4',   //
    '5',  '5',  '5', '6',  '5', '7',  '5', '8',  '5', '9',   //
    '6',  '0',  '6', '1',  '6', '2',  '6', '3',  '6', '4',   //
    '6',  '5',  '6', '6',  '6', '7',  '6', '8',  '6', '9',   //
    '7',  '0',  '7', '1',  '7', '2',  '7', '3',  '7', '4',   //
    '7',  '5',  '7', '6',  '7', '7',  '7', '8',  '7', '9',   //
    '8',  '0',  '8', '1',  '8', '2',  '8', '3',  '8', '4',   //
    '8',  '5',  '8', '6',  '8', '7',  '8', '8',  '8', '9',   //
    '9',  '0',  '9', '1',  '9', '2',  '9', '3',  '9', '4',   //
    '9',  '5',  '9', '6',  '9', '7',  '9', '8',  '9', '9'    //
};

static void Convert2Digits(uint8_t n, char* buffer) {
  std::memcpy(buffer, kRadix100Table + n * 2, 2);
}

// Returns count of digits written.
static uint8_t ConvertHeadDigits(uint8_t n, char* buffer) {
  const uint8_t digit_count = 1 + (n >= 10);
  std::memcpy(buffer, kRadix100HeadTable + n * 2, digit_count);
  return digit_count;
}

static void Convert8Digits(uint32_t n, char* buffer) {
  static constexpr uint32_t kUint32Mask = std::numeric_limits<uint32_t>::max();
  // 281474978 = ceil(2^48 / 1'000'000) + 1
  uint64_t prod = n * 281474978LL;
  prod >>= 16;
  prod += 1;
  Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer);
  prod = (prod & kUint32Mask) * 100;
  Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 2);
  prod = (prod & kUint32Mask) * 100;
  Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 4);
  prod = (prod & kUint32Mask) * 100;
  Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 6);
}

// Returns count of digits written.
uint8_t ConvertUpTo9Digits(uint32_t n, char* buffer) {
  static constexpr uint32_t kUint32Mask = std::numeric_limits<uint32_t>::max();

  if (n >= 100'000'000) {
    // 9 digits.
    // 1441151882 = ceil(2^57 / 100'000'000) + 1
    uint64_t prod = n * 1441151882LL;
    prod >>= 25;

    const uint8_t head_digit = static_cast<uint8_t>(prod >> 32);
    *buffer = '0' + head_digit;

    // Print remaining 8 digits.
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 1);
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 3);
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 5);
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 7);

    return 9;
  }
  if (n >= 1'000'000) {
    // 7 or 8 digits.
    // 281474978 = ceil(2^48 / 1'000'000) + 1
    uint64_t prod = n * 281474978LL;
    prod >>= 16;

    const uint8_t head_digits = static_cast<uint8_t>(prod >> 32);
    const uint8_t head_digit_count = ConvertHeadDigits(head_digits, buffer);
    buffer += head_digit_count;

    // Print remaining 6 digits.
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer);
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 2);
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 4);

    return 6 + head_digit_count;
  }
  if (n >= 10'000) {
    // 5 or 6 digits.
    // 429497 = ceil(2^32 / 10'000)
    uint64_t prod = n * 429497LL;

    const uint8_t head_digits = static_cast<uint8_t>(prod >> 32);
    const uint8_t head_digit_count = ConvertHeadDigits(head_digits, buffer);
    buffer += head_digit_count;

    // Print remaining 4 digits.
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer);
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer + 2);

    return 4 + head_digit_count;
  }
  if (n >= 100) {
    // 3 or 4 digits.
    // 42949673 = ceil(2^32 / 10'000)
    uint64_t prod = n * 42949673LL;

    const uint8_t head_digits = static_cast<uint8_t>(prod >> 32);
    const uint8_t head_digit_count = ConvertHeadDigits(head_digits, buffer);
    buffer += head_digit_count;

    // Print remaining 2 digits.
    prod = (prod & kUint32Mask) * 100;
    Convert2Digits(static_cast<uint8_t>(prod >> 32), buffer);

    return 2 + head_digit_count;
  }
  // 1 or 2 digits.
  return ConvertHeadDigits(n, buffer);
}

// Returns count of digits written.
uint8_t SignificandToChars(uint64_t n, char* buffer) {

  if (n >= 100'000'000) {
    // If we have at least 9 digits, split into 2 blocks. The second one always
    // has exactly 8 digits.
    uint32_t first_block = static_cast<uint32_t>(n / 100'000'000);
    uint32_t second_block = static_cast<uint32_t>(n % 100'000'000);

    uint8_t first_block_digits = ConvertUpTo9Digits(first_block, buffer);
    Convert8Digits(second_block, buffer + first_block_digits);
    return first_block_digits + 8;
  } else {
    return ConvertUpTo9Digits(static_cast<uint32_t>(n), buffer);
  }
}

} // namespace

#if defined(XYO_ASM_ECMA_FORMAT)
extern "C" size_t xyo_ecma_format_asm(
    const char* digits, uint32_t length, int32_t point, char* buffer);
#endif

// Only positive, nonzero, finite doubles reach this function. Scratch's
// NaN, -0, +/-Infinity, sign and integer fast paths stay in jsnum.cpp.
size_t toCharsPositive(double value, char* out) {
    assert(value > 0 && std::isfinite(value));
    const auto decimal = jkj::dragonbox::to_decimal(value);

    char digits[24];
    const int length = SignificandToChars(decimal.significand, digits);
    const int decimal_point = length + decimal.exponent;
    assert(length >= 1 && length <= 17);

#if defined(XYO_ASM_ECMA_FORMAT)
    // Reuse existing ASM decimal layout, skipping scientific text entirely.
    return xyo_ecma_format_asm(
        digits, static_cast<uint32_t>(length), decimal_point, out);
#else
    char* cursor = out;
    if (length <= decimal_point && decimal_point <= 21) {
        std::memcpy(cursor, digits, length);
        cursor += length;
        std::memset(cursor, '0', decimal_point - length);
        cursor += decimal_point - length;
    } else if (0 < decimal_point && decimal_point <= 21) {
        std::memcpy(cursor, digits, decimal_point);
        cursor += decimal_point;
        *cursor++ = '.';
        std::memcpy(cursor, digits + decimal_point, length - decimal_point);
        cursor += length - decimal_point;
    } else if (-6 < decimal_point && decimal_point <= 0) {
        *cursor++ = '0';
        *cursor++ = '.';
        std::memset(cursor, '0', -decimal_point);
        cursor += -decimal_point;
        std::memcpy(cursor, digits, length);
        cursor += length;
    } else {
        *cursor++ = digits[0];
        if (length > 1) {
            *cursor++ = '.';
            std::memcpy(cursor, digits + 1, length - 1);
            cursor += length - 1;
        }
        *cursor++ = 'e';
        int exponent = decimal_point - 1;
        *cursor++ = exponent < 0 ? '-' : '+';
        if (exponent < 0) exponent = -exponent;
        const auto [end, ec] = std::to_chars(cursor, out + 64, exponent);
        assert(ec == std::errc{});
        cursor = end;
    }
    assert(cursor <= out + 64);
    return static_cast<size_t>(cursor - out);
#endif
}

std::string toStringPositive(double value) {
    char buffer[64];
    const size_t length = toCharsPositive(value, buffer);
    return std::string(buffer, length);
}

} // namespace xyo::jsnum::v8_decimal
