// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Time as a value. Every deadline, window and age in this observatory is an
// explicit Instant or Duration; nothing reads a clock implicitly, so an
// assessment produced for a stated evaluation time is reproducible forever.

#ifndef ASSET_HEALTH_TIME_HPP
#define ASSET_HEALTH_TIME_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "asset_health/error.hpp"
#include "asset_health/strong_types.hpp"

namespace asset_health {

/// A signed span of nanoseconds. Negative spans are meaningful (a window that
/// ends before it starts is rejected at construction, but a difference between
/// two instants may be negative), so this type carries a sign.
class ASSET_HEALTH_API Duration {
public:
    constexpr Duration() noexcept = default;
    constexpr explicit Duration(std::int64_t nanoseconds) noexcept : nanoseconds_(nanoseconds) {}

    [[nodiscard]] static constexpr Duration from_nanoseconds(std::int64_t value) noexcept { return Duration(value); }
    [[nodiscard]] static constexpr Duration from_microseconds(std::int64_t value) noexcept {
        return Duration(value * 1000);
    }
    [[nodiscard]] static constexpr Duration from_milliseconds(std::int64_t value) noexcept {
        return Duration(value * 1000 * 1000);
    }
    [[nodiscard]] static constexpr Duration from_seconds(std::int64_t value) noexcept {
        return Duration(value * 1000 * 1000 * 1000);
    }
    [[nodiscard]] static constexpr Duration from_minutes(std::int64_t value) noexcept {
        return Duration(value * 60 * 1000 * 1000 * 1000);
    }
    [[nodiscard]] static constexpr Duration from_hours(std::int64_t value) noexcept {
        return Duration(value * 3600LL * 1000 * 1000 * 1000);
    }
    [[nodiscard]] static constexpr Duration from_days(std::int64_t value) noexcept {
        return Duration(value * 86400LL * 1000 * 1000 * 1000);
    }

    [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept { return nanoseconds_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return nanoseconds_ == 0; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return nanoseconds_ < 0; }

    /// Whole units, truncated toward zero.
    [[nodiscard]] constexpr std::int64_t milliseconds() const noexcept { return nanoseconds_ / 1000000; }
    [[nodiscard]] constexpr std::int64_t seconds() const noexcept { return nanoseconds_ / 1000000000; }
    [[nodiscard]] constexpr std::int64_t minutes() const noexcept { return nanoseconds_ / (60LL * 1000000000LL); }
    [[nodiscard]] constexpr std::int64_t hours() const noexcept { return nanoseconds_ / (3600LL * 1000000000LL); }

    /// Absolute value, saturating at the representation end.
    [[nodiscard]] constexpr Duration magnitude() const noexcept {
        return nanoseconds_ == std::numeric_limits<std::int64_t>::min() ? Duration(std::numeric_limits<std::int64_t>::max())
                                                                      : Duration(nanoseconds_ < 0 ? -nanoseconds_ : nanoseconds_);
    }

    [[nodiscard]] constexpr bool operator==(const Duration& other) const noexcept {
        return nanoseconds_ == other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator!=(const Duration& other) const noexcept { return !(*this == other); }
    [[nodiscard]] constexpr bool operator<(const Duration& other) const noexcept {
        return nanoseconds_ < other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<=(const Duration& other) const noexcept {
        return nanoseconds_ <= other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator>(const Duration& other) const noexcept {
        return nanoseconds_ > other.nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator>=(const Duration& other) const noexcept {
        return nanoseconds_ >= other.nanoseconds_;
    }

    /// Checked addition. Returns std::nullopt on overflow.
    [[nodiscard]] std::optional<Duration> add(const Duration& other) const noexcept;
    [[nodiscard]] std::optional<Duration> subtract(const Duration& other) const noexcept;

    /// Canonical spelling: a decimal count of nanoseconds with an "ns" suffix.
    [[nodiscard]] std::string to_string() const;

    /// Human spelling in whole seconds, for explanations.
    [[nodiscard]] std::string to_seconds_string() const;

private:
    std::int64_t nanoseconds_ = 0;
};

/// An instant in UTC, counted in nanoseconds from the Unix epoch. The epoch is
/// the only reference point; leap seconds are not represented, which is stated
/// here because a durability window measured against this type inherits that.
class ASSET_HEALTH_API Instant {
public:
    constexpr Instant() noexcept = default;
    constexpr explicit Instant(std::int64_t unix_nanoseconds) noexcept : unix_nanoseconds_(unix_nanoseconds) {}

    [[nodiscard]] static constexpr Instant from_unix_nanoseconds(std::int64_t value) noexcept {
        return Instant(value);
    }
    [[nodiscard]] static constexpr Instant from_unix_seconds(std::int64_t value) noexcept {
        return Instant(value * 1000000000LL);
    }
    [[nodiscard]] static constexpr Instant from_unix_milliseconds(std::int64_t value) noexcept {
        return Instant(value * 1000000LL);
    }

    [[nodiscard]] constexpr std::int64_t unix_nanoseconds() const noexcept { return unix_nanoseconds_; }
    [[nodiscard]] constexpr std::int64_t unix_seconds() const noexcept { return unix_nanoseconds_ / 1000000000LL; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return unix_nanoseconds_ == 0; }

    [[nodiscard]] constexpr bool operator==(const Instant& other) const noexcept {
        return unix_nanoseconds_ == other.unix_nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator!=(const Instant& other) const noexcept { return !(*this == other); }
    [[nodiscard]] constexpr bool operator<(const Instant& other) const noexcept {
        return unix_nanoseconds_ < other.unix_nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator<=(const Instant& other) const noexcept {
        return unix_nanoseconds_ <= other.unix_nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator>(const Instant& other) const noexcept {
        return unix_nanoseconds_ > other.unix_nanoseconds_;
    }
    [[nodiscard]] constexpr bool operator>=(const Instant& other) const noexcept {
        return unix_nanoseconds_ >= other.unix_nanoseconds_;
    }

    /// this - earlier, as a signed duration. Saturates on overflow rather than
    /// wrapping, because an age is compared against a threshold, not summed.
    [[nodiscard]] Duration minus(const Instant& earlier) const noexcept;

    /// Checked shift by p delta. Returns std::nullopt on overflow.
    [[nodiscard]] std::optional<Instant> shifted(const Duration& delta) const noexcept;

    /// Canonical "YYYY-MM-DDTHH:MM:SS.fffffffffZ" spelling in UTC.
    [[nodiscard]] std::string to_string() const;

    /// Canonical spelling with millisecond precision, for logs and CLI output.
    [[nodiscard]] std::string to_millisecond_string() const;

private:
    std::int64_t unix_nanoseconds_ = 0;
};

/// Parses a UTC timestamp. Accepts "YYYY-MM-DDTHH:MM:SS" optionally followed by
/// ".fraction" of up to nine digits and a "Z"; the offset form "+00:00" is also
/// accepted because it denotes the same instant. Any other offset is rejected:
/// this observatory stores one time base, UTC.
[[nodiscard]] ASSET_HEALTH_API std::optional<Instant> parse_instant(std::string_view text) noexcept;

/// Parses a duration written as a decimal count with a unit suffix from
/// {ns, us, ms, s, m, h, d}. A sign is accepted; the empty string is rejected.
[[nodiscard]] ASSET_HEALTH_API std::optional<Duration> parse_duration(std::string_view text) noexcept;

/// The time source an observatory reads. Every evaluation takes its "now" from
/// here, so a caller that wants a reproducible answer supplies a fixed clock.
class ASSET_HEALTH_API Clock {
public:
    Clock() = default;
    virtual ~Clock();
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    /// The current instant in UTC.
    [[nodiscard]] virtual Instant now() const = 0;
};

/// The operating system's wall clock, truncated to nanoseconds.
class ASSET_HEALTH_API SystemClock final : public Clock {
public:
    [[nodiscard]] Instant now() const override;
};

/// A clock that returns a fixed instant until it is advanced. Used by tests and
/// by the command line when an evaluation time is supplied.
class ASSET_HEALTH_API ManualClock final : public Clock {
public:
    explicit ManualClock(Instant start) noexcept : current_(start) {}

    [[nodiscard]] Instant now() const override { return current_; }

    void set(Instant value) noexcept { current_ = value; }
    void advance(Duration delta) noexcept;

private:
    Instant current_;
};

}  // namespace asset_health

#endif  // ASSET_HEALTH_TIME_HPP
