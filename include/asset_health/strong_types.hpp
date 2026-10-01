// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Strongly typed scalars. None of these types converts implicitly to any other,
// and none of them carries an "invalid" sentinel: absence is modelled with
// std::optional at the call site.
//
// A value of one of these types is canonical. There is exactly one textual
// spelling per value, it round-trips, and input that is not already in that
// spelling is rejected rather than repaired.

#ifndef ASSET_HEALTH_STRONG_TYPES_HPP
#define ASSET_HEALTH_STRONG_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "asset_health/error.hpp"

namespace asset_health {

namespace detail {

/// Common implementation of a strictly monotonic unsigned counter. Two counters
/// of different kinds are different types, so mixing them is a compile error
/// rather than a wrong number.
template <typename Tag, typename Rep>
class Counter {
public:
    using rep_type = Rep;

    constexpr Counter() noexcept = default;
    constexpr explicit Counter(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

    [[nodiscard]] constexpr bool operator==(const Counter& other) const noexcept {
        return value_ == other.value_;
    }
    [[nodiscard]] constexpr bool operator!=(const Counter& other) const noexcept {
        return value_ != other.value_;
    }
    [[nodiscard]] constexpr bool operator<(const Counter& other) const noexcept {
        return value_ < other.value_;
    }
    [[nodiscard]] constexpr bool operator<=(const Counter& other) const noexcept {
        return value_ <= other.value_;
    }
    [[nodiscard]] constexpr bool operator>(const Counter& other) const noexcept {
        return value_ > other.value_;
    }
    [[nodiscard]] constexpr bool operator>=(const Counter& other) const noexcept {
        return value_ >= other.value_;
    }

    /// Strictly increasing successor. Returns std::nullopt at the end of the
    /// representation instead of wrapping, so a counter never silently restarts.
    [[nodiscard]] constexpr std::optional<Counter> next() const noexcept {
        if (value_ == std::numeric_limits<Rep>::max()) {
            return std::nullopt;
        }
        return Counter(static_cast<Rep>(value_ + 1));
    }

    /// Saturating subtraction: the result is never negative.
    [[nodiscard]] constexpr Rep distance_from(const Counter& earlier) const noexcept {
        return value_ >= earlier.value() ? static_cast<Rep>(value_ - earlier.value()) : Rep{0};
    }

    /// Parses the canonical decimal spelling. Leading zeros beyond a single "0",
    /// signs, whitespace and every non-digit character are rejected.
    [[nodiscard]] static std::optional<Counter> parse(std::string_view text) noexcept {
        if (text.empty() || text.size() > 20) {
            return std::nullopt;
        }
        if (text.size() > 1 && text.front() == '0') {
            return std::nullopt;
        }
        Rep value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            const Rep digit = static_cast<Rep>(c - '0');
            if (value > static_cast<Rep>((std::numeric_limits<Rep>::max() - digit) / 10)) {
                return std::nullopt;
            }
            value = static_cast<Rep>(value * 10 + digit);
        }
        return Counter(value);
    }

    [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

private:
    Rep value_ = 0;
};

/// 128-bit identifier bytes shared by every canonical identifier type.
using UuidBytes = std::array<std::uint8_t, 16>;

}  // namespace detail

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

/// Bumped every time a source process restarts its evidence stream. An
/// observation stamped with an epoch lower than the newest epoch seen for that
/// source is a replay of a dead stream and is refused.
struct StreamEpochTag;
using StreamEpoch = detail::Counter<StreamEpochTag, std::uint64_t>;

/// Per-epoch sequence number of an observation inside one source stream. It
/// advances by exactly one per observation and must not repeat inside an epoch.
struct StreamSequenceTag;
using StreamSequence = detail::Counter<StreamSequenceTag, std::uint64_t>;

/// Epoch of the durable store. Every successful open publishes a strictly
/// greater epoch; a writer holding a token minted under an earlier epoch is
/// fenced out, including across a process restart.
struct StoreEpochTag;
using StoreEpoch = detail::Counter<StoreEpochTag, std::uint64_t>;

/// Monotonic counter identifying one durable generation of the store. A
/// published generation always carries a strictly greater sequence than the
/// generation it superseded.
struct GenerationSequenceTag;
using GenerationSequence = detail::Counter<GenerationSequenceTag, std::uint64_t>;

/// Per-asset counter of published assessments. Revision 1 is the first
/// assessment published for an asset and generation.
struct AssessmentRevisionTag;
using AssessmentRevision = detail::Counter<AssessmentRevisionTag, std::uint64_t>;

/// Format version of the durable store layout.
struct StoreFormatVersionTag;
using StoreFormatVersion = detail::Counter<StoreFormatVersionTag, std::uint32_t>;

/// Version of a health policy. A published assessment names the policy and this
/// version, so a change of thresholds is visible in every explanation.
struct PolicyVersionTag;
using PolicyVersion = detail::Counter<PolicyVersionTag, std::uint32_t>;

/// Number of observations one fault identity has accumulated. Reported, never
/// used to inflate severity.
struct OccurrenceCountTag;
using OccurrenceCount = detail::Counter<OccurrenceCountTag, std::uint64_t>;

// ---------------------------------------------------------------------------
// Exact rational arithmetic
// ---------------------------------------------------------------------------

/// An exact rational number with a 64-bit numerator and a strictly positive
/// 64-bit denominator, always stored in lowest terms with the denominator
/// positive. Every operation is checked: a result that would overflow returns
/// std::nullopt instead of a wrong number. Exactness is what makes a risk total
/// reproducible on every machine and comparable in a test.
class ASSET_HEALTH_API Rational {
public:
    constexpr Rational() noexcept = default;

    /// Builds the rational from a numerator and denominator. The denominator is
    /// normalised to be positive and the pair is reduced; a zero denominator or
    /// the most negative numerator with a denominator of -1 is rejected.
    [[nodiscard]] static std::optional<Rational> make(std::int64_t numerator, std::int64_t denominator) noexcept;

    /// Builds a whole number.
    [[nodiscard]] static constexpr Rational from_integer(std::int64_t value) noexcept {
        return Rational(value, 1);
    }

    /// Builds the exact rational value/1000, i.e. the rational represented by a
    /// fixed-point milli-unit.
    [[nodiscard]] static std::optional<Rational> from_milli(std::int64_t milli) noexcept;

    [[nodiscard]] constexpr std::int64_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] constexpr std::int64_t denominator() const noexcept { return denominator_; }

    [[nodiscard]] bool is_zero() const noexcept { return numerator_ == 0; }
    [[nodiscard]] bool is_negative() const noexcept { return numerator_ < 0; }

    [[nodiscard]] std::optional<Rational> add(const Rational& other) const noexcept;
    [[nodiscard]] std::optional<Rational> subtract(const Rational& other) const noexcept;
    [[nodiscard]] std::optional<Rational> multiply(const Rational& other) const noexcept;
    [[nodiscard]] std::optional<Rational> divide(const Rational& other) const noexcept;

    /// Sign of (this - other), the only comparison the risk model needs.
    [[nodiscard]] int compare(const Rational& other) const noexcept;

    [[nodiscard]] bool operator==(const Rational& other) const noexcept {
        return numerator_ == other.numerator_ && denominator_ == other.denominator_;
    }
    [[nodiscard]] bool operator!=(const Rational& other) const noexcept { return !(*this == other); }
    [[nodiscard]] bool operator<(const Rational& other) const noexcept { return compare(other) < 0; }
    [[nodiscard]] bool operator<=(const Rational& other) const noexcept { return compare(other) <= 0; }
    [[nodiscard]] bool operator>(const Rational& other) const noexcept { return compare(other) > 0; }
    [[nodiscard]] bool operator>=(const Rational& other) const noexcept { return compare(other) >= 0; }

    /// Canonical "numerator/denominator" spelling; a whole number is "n/1".
    [[nodiscard]] std::string to_string() const;

    /// Rounded decimal spelling with p places digits after the point, used for
    /// presentation only. Negative zero is never produced.
    [[nodiscard]] std::string to_decimal_string(unsigned places) const;

    /// Truncating conversion to milli-units, saturating at the representation ends.
    [[nodiscard]] std::int64_t to_milli_truncated() const noexcept;

private:
    constexpr Rational(std::int64_t numerator, std::int64_t denominator) noexcept
        : numerator_(numerator), denominator_(denominator) {}

    std::int64_t numerator_ = 0;
    std::int64_t denominator_ = 1;
};

// ---------------------------------------------------------------------------
// Canonical text values
// ---------------------------------------------------------------------------

/// A canonical lowercase identifier: 1..64 characters of [a-z0-9], optionally
/// separated by '.', '-' or '_', never starting or ending with a separator and
/// never containing two separators in a row. A vendor spelling that does not
/// already match is rejected, not folded: mapping a vendor's spelling onto the
/// canonical one is the producer's job, and doing it here would hide a contract
/// mismatch.
class ASSET_HEALTH_API Token {
public:
    Token() = default;

    [[nodiscard]] static std::optional<Token> parse(std::string_view text) noexcept;

    /// Builds a token from a literal the caller guarantees canonical. The
    /// assertion is checked: a non-canonical literal returns std::nullopt rather
    /// than producing an invalid token.
    [[nodiscard]] static std::optional<Token> from_canonical(std::string_view text) noexcept { return parse(text); }

    [[nodiscard]] const std::string& str() const noexcept { return text_; }
    [[nodiscard]] std::string_view view() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    [[nodiscard]] bool operator==(const Token& other) const noexcept { return text_ == other.text_; }
    [[nodiscard]] bool operator!=(const Token& other) const noexcept { return !(*this == other); }
    [[nodiscard]] bool operator<(const Token& other) const noexcept { return text_ < other.text_; }
    [[nodiscard]] bool operator<=(const Token& other) const noexcept { return text_ <= other.text_; }
    [[nodiscard]] bool operator>(const Token& other) const noexcept { return text_ > other.text_; }
    [[nodiscard]] bool operator>=(const Token& other) const noexcept { return text_ >= other.text_; }

private:
    std::string text_;
};

/// A Token carrying a phantom tag, so two identifier kinds that share the same
/// grammar are still different types and cannot be interchanged by accident.
template <typename Tag>
class TaggedToken {
public:
    TaggedToken() = default;

    [[nodiscard]] static std::optional<TaggedToken> parse(std::string_view text) noexcept {
        const auto token = Token::parse(text);
        if (!token.has_value()) {
            return std::nullopt;
        }
        return TaggedToken(*token);
    }

    [[nodiscard]] const std::string& str() const noexcept { return token_.str(); }
    [[nodiscard]] std::string_view view() const noexcept { return token_.view(); }
    [[nodiscard]] bool empty() const noexcept { return token_.empty(); }

    [[nodiscard]] bool operator==(const TaggedToken& other) const noexcept { return token_ == other.token_; }
    [[nodiscard]] bool operator!=(const TaggedToken& other) const noexcept { return !(*this == other); }
    [[nodiscard]] bool operator<(const TaggedToken& other) const noexcept { return token_ < other.token_; }
    [[nodiscard]] bool operator<=(const TaggedToken& other) const noexcept { return token_ <= other.token_; }
    [[nodiscard]] bool operator>(const TaggedToken& other) const noexcept { return token_ > other.token_; }
    [[nodiscard]] bool operator>=(const TaggedToken& other) const noexcept { return token_ >= other.token_; }

private:
    explicit TaggedToken(Token token) noexcept : token_(std::move(token)) {}

    Token token_;
};

/// A version or baseline label: 1..64 printable, non-space characters from
/// [A-Za-z0-9._+-]. Case is significant and preserved: "2.1.0" and "2.1.0-rc1"
/// are different versions and neither is folded into the other.
class ASSET_HEALTH_API VersionText {
public:
    VersionText() = default;

    [[nodiscard]] static std::optional<VersionText> parse(std::string_view text) noexcept;

    [[nodiscard]] const std::string& str() const noexcept { return text_; }
    [[nodiscard]] std::string_view view() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    [[nodiscard]] bool operator==(const VersionText& other) const noexcept { return text_ == other.text_; }
    [[nodiscard]] bool operator!=(const VersionText& other) const noexcept { return !(*this == other); }
    [[nodiscard]] bool operator<(const VersionText& other) const noexcept { return text_ < other.text_; }

private:
    std::string text_;
};

/// Short free-form text used for human explanations. Control characters are
/// rejected; the text is otherwise preserved exactly, because it is evidence of
/// what a producer said.
class ASSET_HEALTH_API DetailText {
public:
    DetailText() = default;

    [[nodiscard]] static std::optional<DetailText> parse(std::string_view text, std::size_t max_length) noexcept;

    [[nodiscard]] const std::string& str() const noexcept { return text_; }
    [[nodiscard]] std::string_view view() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

    [[nodiscard]] bool operator==(const DetailText& other) const noexcept { return text_ == other.text_; }
    [[nodiscard]] bool operator!=(const DetailText& other) const noexcept { return !(*this == other); }

private:
    std::string text_;
};

/// True when every byte of p text is canonical-token material.
[[nodiscard]] ASSET_HEALTH_API bool is_token_text(std::string_view text) noexcept;

/// True when every byte of p text is version-label material.
[[nodiscard]] ASSET_HEALTH_API bool is_version_text(std::string_view text) noexcept;

/// True when every byte of p text is printable and none is a control character.
[[nodiscard]] ASSET_HEALTH_API bool is_printable_text(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Canonical 128-bit identifiers
// ---------------------------------------------------------------------------

/// Parses the canonical lowercase hyphenated "8-4-4-4-12" spelling. Uppercase,
/// braces, a URN prefix and the unhyphenated form are all rejected.
[[nodiscard]] ASSET_HEALTH_API std::optional<detail::UuidBytes> parse_uuid_text(std::string_view text) noexcept;

/// Renders the canonical lowercase hyphenated spelling.
[[nodiscard]] ASSET_HEALTH_API std::string format_uuid_text(const detail::UuidBytes& bytes);

/// Renders fixed-width lowercase hex without hyphens, for compact ordering keys.
[[nodiscard]] ASSET_HEALTH_API std::string format_uuid_compact(const detail::UuidBytes& bytes);

/// Fills p out from the operating system entropy source. Returns false when the
/// platform entropy source is unavailable; the caller reports that instead of
/// substituting a predictable value.
[[nodiscard]] ASSET_HEALTH_API bool generate_uuid_v4(detail::UuidBytes& out) noexcept;

/// True when the identifier carries the version-4 and RFC-4122 variant bits.
[[nodiscard]] ASSET_HEALTH_API bool is_uuid_v4(const detail::UuidBytes& bytes) noexcept;

/// Checks a multiplication and an addition in one step: returns std::nullopt on
/// overflow so a caller never observes a wrapped quantity.
[[nodiscard]] inline std::optional<std::int64_t> checked_mul_add(std::int64_t a, std::int64_t b,
                                                                std::int64_t c) noexcept {
    std::int64_t product = 0;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_mul_overflow(a, b, &product)) {
        return std::nullopt;
    }
    std::int64_t sum = 0;
    if (__builtin_add_overflow(product, c, &sum)) {
        return std::nullopt;
    }
    return sum;
#else
    // Portable fallback: refuse the multiplication when the operands cannot
    // produce a representable product. The comparison is done in the widest
    // available integer so the check itself cannot overflow.
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (a != 0) {
        if (a == -1) {
            if (b == kMin) {
                return std::nullopt;
            }
            product = -b;
        } else if (b > kMax / a || b < kMin / a) {
            return std::nullopt;
        } else {
            product = a * b;
        }
    }
    if ((c > 0 && product > kMax - c) || (c < 0 && product < kMin - c)) {
        return std::nullopt;
    }
    return product + c;
#endif
}

/// Checks an addition, returning std::nullopt on overflow.
[[nodiscard]] inline std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
        return std::nullopt;
    }
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
        return std::nullopt;
    }
    return a + b;
}

}  // namespace asset_health

template <>
struct std::hash<::asset_health::Token> {
    [[nodiscard]] std::size_t operator()(const ::asset_health::Token& token) const noexcept {
        return std::hash<std::string>{}(token.str());
    }
};

#endif  // ASSET_HEALTH_STRONG_TYPES_HPP
