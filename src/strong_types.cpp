// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/strong_types.hpp"

#include <algorithm>
#include <string_view>

#include "asset_health/limits.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace asset_health {
namespace {

[[nodiscard]] std::int64_t gcd_i64(std::int64_t a, std::int64_t b) noexcept {
    // Both operands are made non-negative before the loop, so the subtraction
    // form cannot overflow for the values a rational can hold.
    if (a < 0) {
        a = -a;
    }
    if (b < 0) {
        b = -b;
    }
    while (b != 0) {
        const std::int64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

/// Floor division for a positive divisor, which is what the exact comparison
/// below needs in order to keep its remainders non-negative.
[[nodiscard]] std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) noexcept {
    std::int64_t quotient = numerator / denominator;
    if (numerator % denominator != 0 && numerator < 0) {
        quotient -= 1;
    }
    return quotient;
}

/// Sign of (a/b - c/d) for positive b and d. Exact, and free of the overflow a
/// cross multiplication of 64-bit operands would have: the remainders shrink on
/// every recursion, so this is the Euclidean algorithm with a comparison at each
/// step.
[[nodiscard]] int compare_fractions(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d) noexcept {
    const std::int64_t q1 = floor_div(a, b);
    const std::int64_t q2 = floor_div(c, d);
    if (q1 != q2) {
        return q1 < q2 ? -1 : 1;
    }
    const std::int64_t r1 = a - q1 * b;
    const std::int64_t r2 = c - q2 * d;
    if (r1 == 0 && r2 == 0) {
        return 0;
    }
    if (r1 == 0) {
        return -1;
    }
    if (r2 == 0) {
        return 1;
    }
    // Both remainders are positive, so both fractions lie strictly between the
    // same two integers. Comparing them is comparing their reciprocals the other
    // way round: x < y exactly when 1/x > 1/y. The remainders shrink on every
    // step, so this terminates and never multiplies two 64-bit values together.
    return compare_fractions(d, r2, b, r1);
}

[[nodiscard]] bool is_lower_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

[[nodiscard]] bool is_separator(char c) noexcept { return c == '.' || c == '-' || c == '_'; }

[[nodiscard]] bool is_version_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '+' || c == '_';
}

[[nodiscard]] int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Rational
// ---------------------------------------------------------------------------

std::optional<Rational> Rational::make(std::int64_t numerator, std::int64_t denominator) noexcept {
    if (denominator == 0) {
        return std::nullopt;
    }
    if (numerator == 0) {
        return Rational(0, 1);
    }
    if (denominator < 0) {
        if (numerator == std::numeric_limits<std::int64_t>::min() ||
            denominator == std::numeric_limits<std::int64_t>::min()) {
            return std::nullopt;
        }
        numerator = -numerator;
        denominator = -denominator;
    }
    const std::int64_t divisor = gcd_i64(numerator, denominator);
    return Rational(numerator / divisor, denominator / divisor);
}

std::optional<Rational> Rational::from_milli(std::int64_t milli) noexcept { return make(milli, 1000); }

std::optional<Rational> Rational::add(const Rational& other) const noexcept {
    // Reduce before multiplying so that a sum of two representable rationals
    // overflows only when the exact result genuinely does not fit.
    const std::int64_t divisor = gcd_i64(denominator_, other.denominator_);
    const std::int64_t left_scale = other.denominator_ / divisor;
    const std::int64_t right_scale = denominator_ / divisor;
    const auto left = checked_mul_add(numerator_, left_scale, 0);
    if (!left.has_value()) {
        return std::nullopt;
    }
    const auto right = checked_mul_add(other.numerator_, right_scale, 0);
    if (!right.has_value()) {
        return std::nullopt;
    }
    const auto numerator = checked_add(*left, *right);
    if (!numerator.has_value()) {
        return std::nullopt;
    }
    const auto denominator = checked_mul_add(left_scale, denominator_, 0);
    if (!denominator.has_value()) {
        return std::nullopt;
    }
    return make(*numerator, *denominator);
}

std::optional<Rational> Rational::subtract(const Rational& other) const noexcept {
    const auto negated = make(-other.numerator_, other.denominator_);
    if (!negated.has_value()) {
        return std::nullopt;
    }
    return add(*negated);
}

std::optional<Rational> Rational::multiply(const Rational& other) const noexcept {
    const std::int64_t first = gcd_i64(numerator_, other.denominator_);
    const std::int64_t second = gcd_i64(other.numerator_, denominator_);
    const auto numerator = checked_mul_add(numerator_ / first, other.numerator_ / second, 0);
    if (!numerator.has_value()) {
        return std::nullopt;
    }
    const auto denominator = checked_mul_add(denominator_ / second, other.denominator_ / first, 0);
    if (!denominator.has_value()) {
        return std::nullopt;
    }
    return make(*numerator, *denominator);
}

std::optional<Rational> Rational::divide(const Rational& other) const noexcept {
    if (other.numerator_ == 0) {
        return std::nullopt;
    }
    const auto reciprocal = make(other.denominator_, other.numerator_);
    if (!reciprocal.has_value()) {
        return std::nullopt;
    }
    return multiply(*reciprocal);
}

int Rational::compare(const Rational& other) const noexcept {
    return compare_fractions(numerator_, denominator_, other.numerator_, other.denominator_);
}

std::string Rational::to_string() const {
    return std::to_string(numerator_) + "/" + std::to_string(denominator_);
}

std::string Rational::to_decimal_string(unsigned places) const {
    if (places == 0 || places > 9) {
        places = 3;
    }
    std::int64_t scale = 1;
    for (unsigned index = 0; index < places; ++index) {
        scale *= 10;
    }
    const auto scaled = checked_mul_add(numerator_, scale, 0);
    std::int64_t scaled_value = 0;
    if (scaled.has_value()) {
        scaled_value = *scaled / denominator_;
    } else {
        // The scaled numerator does not fit; fall back to dividing first, which
        // loses digits beyond the requested precision but never invents them.
        scaled_value = (numerator_ / denominator_) * scale;
        const auto remainder = checked_mul_add(numerator_ % denominator_, scale, 0);
        if (remainder.has_value()) {
            scaled_value = checked_add(scaled_value, *remainder / denominator_).value_or(scaled_value);
        }
    }
    const bool negative = scaled_value < 0;
    const std::int64_t magnitude = negative ? -scaled_value : scaled_value;
    const std::int64_t whole = magnitude / scale;
    const std::int64_t fraction = magnitude % scale;
    std::string text;
    if (negative && (whole != 0 || fraction != 0)) {
        text.push_back('-');
    }
    text.append(std::to_string(whole));
    if (places > 0) {
        text.push_back('.');
        std::string digits = std::to_string(fraction);
        digits.insert(digits.begin(), static_cast<std::size_t>(places) - std::min<std::size_t>(digits.size(), places),
                      '0');
        text.append(digits);
    }
    return text;
}

std::int64_t Rational::to_milli_truncated() const noexcept {
    const auto scaled = checked_mul_add(numerator_, 1000, 0);
    if (scaled.has_value()) {
        return *scaled / denominator_;
    }
    return numerator_ / denominator_ * 1000;
}

// ---------------------------------------------------------------------------
// Canonical text values
// ---------------------------------------------------------------------------

bool is_token_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxTokenLength) {
        return false;
    }
    if (!is_lower_alnum(text.front()) || !is_lower_alnum(text.back())) {
        return false;
    }
    bool previous_separator = false;
    for (const char c : text) {
        if (is_lower_alnum(c)) {
            previous_separator = false;
            continue;
        }
        if (!is_separator(c) || previous_separator) {
            return false;
        }
        previous_separator = true;
    }
    return true;
}

bool is_version_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxVersionTextLength) {
        return false;
    }
    if (!is_version_char(text.front()) || !is_version_char(text.back())) {
        return false;
    }
    for (const char c : text) {
        if (!is_version_char(c)) {
            return false;
        }
    }
    return true;
}

bool is_printable_text(std::string_view text) noexcept {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

std::optional<Token> Token::parse(std::string_view text) noexcept {
    if (!is_token_text(text)) {
        return std::nullopt;
    }
    Token token;
    token.text_.assign(text);
    return token;
}

std::optional<VersionText> VersionText::parse(std::string_view text) noexcept {
    if (!is_version_text(text)) {
        return std::nullopt;
    }
    VersionText value;
    value.text_.assign(text);
    return value;
}

std::optional<DetailText> DetailText::parse(std::string_view text, std::size_t max_length) noexcept {
    if (text.size() > max_length || !is_printable_text(text)) {
        return std::nullopt;
    }
    DetailText value;
    value.text_.assign(text);
    return value;
}

// ---------------------------------------------------------------------------
// Canonical 128-bit identifiers
// ---------------------------------------------------------------------------

std::optional<detail::UuidBytes> parse_uuid_text(std::string_view text) noexcept {
    if (text.size() != 36) {
        return std::nullopt;
    }
    detail::UuidBytes bytes{};
    std::size_t byte_index = 0;
    int high = -1;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (c != '-') {
                return std::nullopt;
            }
            continue;
        }
        const int value = hex_value(c);
        if (value < 0) {
            return std::nullopt;
        }
        if (high < 0) {
            high = value;
            continue;
        }
        bytes[byte_index] = static_cast<std::uint8_t>((high << 4) | value);
        ++byte_index;
        high = -1;
    }
    if (byte_index != 16 || high != -1) {
        return std::nullopt;
    }
    return bytes;
}

std::string format_uuid_text(const detail::UuidBytes& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text.push_back('-');
        }
        text.push_back(kHex[(bytes[index] >> 4) & 0x0F]);
        text.push_back(kHex[bytes[index] & 0x0F]);
    }
    return text;
}

std::string format_uuid_compact(const detail::UuidBytes& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(32);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kHex[(byte >> 4) & 0x0F]);
        text.push_back(kHex[byte & 0x0F]);
    }
    return text;
}

bool is_uuid_v4(const detail::UuidBytes& bytes) noexcept {
    return (bytes[6] & 0xF0) == 0x40 && (bytes[8] & 0xC0) == 0x80;
}

bool generate_uuid_v4(detail::UuidBytes& out) noexcept {
    detail::UuidBytes bytes{};
#if defined(_WIN32)
    // BCryptGenRandom is part of the operating system. It is a system call, not a
    // third-party dependency, and it is the only entropy source used here.
    const NTSTATUS status =
        ::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return false;
    }
#else
    const int descriptor = ::open("/dev/urandom", O_RDONLY);
    if (descriptor < 0) {
        return false;
    }
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const ssize_t got = ::read(descriptor, bytes.data() + filled, bytes.size() - filled);
        if (got <= 0) {
            ::close(descriptor);
            return false;
        }
        filled += static_cast<std::size_t>(got);
    }
    ::close(descriptor);
#endif
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    out = bytes;
    return true;
}

}  // namespace asset_health
