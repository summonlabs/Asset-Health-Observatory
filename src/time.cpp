// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace asset_health {
namespace {

struct CivilDate {
    std::int64_t year = 1970;
    unsigned month = 1;
    unsigned day = 1;
};

/// Days from 1970-01-01 to the given civil date, proleptic Gregorian. This is
/// the standard days-from-civil algorithm: exact for every year a 64-bit count
/// of nanoseconds can reach.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    const std::int64_t month_value = static_cast<std::int64_t>(month);
    const std::int64_t day_value = static_cast<std::int64_t>(day);
    year -= month_value <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const std::int64_t year_of_era = year - era * 400;
    const std::int64_t day_of_year = (153 * (month_value + (month_value > 2 ? -3 : 9)) + 2) / 5 + day_value - 1;
    const std::int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

/// The civil date a day count names.
[[nodiscard]] CivilDate civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460u + day_of_era / 36524u - day_of_era / 146096u) / 365u;
    std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
    const unsigned day_of_year = day_of_era - (365u * year_of_era + year_of_era / 4u - year_of_era / 100u);
    const unsigned month_prime = (5u * day_of_year + 2u) / 153u;
    const unsigned day = day_of_year - (153u * month_prime + 2u) / 5u + 1u;
    const unsigned month = month_prime + (month_prime < 10u ? 3u : static_cast<unsigned>(-9));
    year += month <= 2u ? 1 : 0;
    CivilDate date;
    date.year = year;
    date.month = month;
    date.day = day;
    return date;
}

[[nodiscard]] bool is_leap_year(std::int64_t year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
    static constexpr std::array<unsigned, 12> kMonthLengths{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kMonthLengths[month - 1];
}

[[nodiscard]] bool read_fixed_digits(std::string_view text, std::size_t offset, std::size_t count,
                                     int& out) noexcept {
    if (offset + count > text.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char c = text[offset + index];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

[[nodiscard]] std::string format_instant(std::int64_t unix_nanoseconds, unsigned fractional_digits) {
    const std::int64_t seconds = unix_nanoseconds / 1000000000LL;
    std::int64_t nanos = unix_nanoseconds % 1000000000LL;
    if (nanos < 0) {
        nanos += 1000000000LL;
    }
    std::int64_t days = seconds / 86400;
    std::int64_t second_of_day = seconds % 86400;
    if (second_of_day < 0) {
        second_of_day += 86400;
        days -= 1;
    }
    const CivilDate date = civil_from_days(days);

    auto pad = [](std::int64_t value, std::size_t width) {
        std::string digits = std::to_string(value);
        if (digits.size() < width) {
            digits.insert(digits.begin(), width - digits.size(), '0');
        }
        return digits;
    };

    std::string text;
    text.reserve(35);
    text.append(pad(date.year, 4));
    text.push_back('-');
    text.append(pad(date.month, 2));
    text.push_back('-');
    text.append(pad(date.day, 2));
    text.push_back('T');
    text.append(pad(second_of_day / 3600, 2));
    text.push_back(':');
    text.append(pad((second_of_day % 3600) / 60, 2));
    text.push_back(':');
    text.append(pad(second_of_day % 60, 2));
    if (fractional_digits > 0) {
        std::string nanos_text = pad(nanos, 9);
        text.push_back('.');
        text.append(nanos_text.substr(0, fractional_digits));
    }
    text.push_back('Z');
    return text;
}

}  // namespace

std::optional<Duration> Duration::add(const Duration& other) const noexcept {
    const auto sum = checked_add(nanoseconds_, other.nanoseconds_);
    if (!sum.has_value()) {
        return std::nullopt;
    }
    return Duration(*sum);
}

std::optional<Duration> Duration::subtract(const Duration& other) const noexcept {
    if (other.nanoseconds_ == std::numeric_limits<std::int64_t>::min()) {
        return std::nullopt;
    }
    return add(Duration(-other.nanoseconds_));
}

std::string Duration::to_string() const { return std::to_string(nanoseconds_) + "ns"; }

std::string Duration::to_seconds_string() const {
    const std::int64_t seconds = nanoseconds_ / 1000000000LL;
    const std::int64_t nanos = nanoseconds_ % 1000000000LL;
    if (nanos == 0) {
        return std::to_string(seconds) + "s";
    }
    const bool negative = nanoseconds_ < 0;
    const std::int64_t magnitude_nanos = negative ? -nanos : nanos;
    std::string fraction = std::to_string(magnitude_nanos);
    fraction.insert(fraction.begin(), 9u - fraction.size(), '0');
    while (!fraction.empty() && fraction.back() == '0') {
        fraction.pop_back();
    }
    std::string text = std::to_string(seconds);
    if (negative && seconds == 0) {
        text.clear();
        text.push_back('-');
        text.append("0");
    }
    text.push_back('.');
    text.append(fraction);
    text.push_back('s');
    return text;
}

Duration Instant::minus(const Instant& earlier) const noexcept {
    const auto difference = checked_add(unix_nanoseconds_, -earlier.unix_nanoseconds_);
    if (difference.has_value()) {
        return Duration(*difference);
    }
    return Duration(unix_nanoseconds_ >= earlier.unix_nanoseconds_ ? std::numeric_limits<std::int64_t>::max()
                                                                  : std::numeric_limits<std::int64_t>::min());
}

std::optional<Instant> Instant::shifted(const Duration& delta) const noexcept {
    const auto shifted_value = checked_add(unix_nanoseconds_, delta.nanoseconds());
    if (!shifted_value.has_value()) {
        return std::nullopt;
    }
    return Instant(*shifted_value);
}

std::string Instant::to_string() const { return format_instant(unix_nanoseconds_, 9); }

std::string Instant::to_millisecond_string() const { return format_instant(unix_nanoseconds_, 3); }

std::optional<Instant> parse_instant(std::string_view text) noexcept {
    if (text.size() < 19) {
        return std::nullopt;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!read_fixed_digits(text, 0, 4, year) || text[4] != '-' || !read_fixed_digits(text, 5, 2, month) ||
        text[7] != '-' || !read_fixed_digits(text, 8, 2, day) || text[10] != 'T' ||
        !read_fixed_digits(text, 11, 2, hour) || text[13] != ':' || !read_fixed_digits(text, 14, 2, minute) ||
        text[16] != ':' || !read_fixed_digits(text, 17, 2, second)) {
        return std::nullopt;
    }
    std::size_t index = 19;
    std::int64_t nanos = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        std::size_t digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (digits < 9) {
                nanos = nanos * 10 + (text[index] - '0');
                ++digits;
            } else {
                // More precision than nanoseconds is refused rather than
                // truncated: a timestamp that lies about its own precision is
                // worse than one that is rejected.
                return std::nullopt;
            }
            ++index;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        for (std::size_t pad = digits; pad < 9; ++pad) {
            nanos *= 10;
        }
    }
    const std::string_view offset = text.substr(index);
    if (offset == "Z") {
        // UTC, nothing to do.
    } else if (offset == "+00:00" || offset == "-00:00") {
        // The zero offset, which names the same instant.
    } else {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || static_cast<unsigned>(day) > days_in_month(year, static_cast<unsigned>(month)) ||
        hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const auto seconds = checked_mul_add(days, 86400, static_cast<std::int64_t>(hour) * 3600 +
                                                          static_cast<std::int64_t>(minute) * 60 + second);
    if (!seconds.has_value()) {
        return std::nullopt;
    }
    const auto nanoseconds = checked_mul_add(*seconds, 1000000000, nanos);
    if (!nanoseconds.has_value()) {
        return std::nullopt;
    }
    return Instant(*nanoseconds);
}

std::optional<Duration> parse_duration(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    bool negative = false;
    std::size_t index = 0;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        index = 1;
    }
    std::size_t digits_end = index;
    while (digits_end < text.size() && text[digits_end] >= '0' && text[digits_end] <= '9') {
        ++digits_end;
    }
    if (digits_end == index) {
        return std::nullopt;
    }
    std::int64_t whole = 0;
    for (std::size_t cursor = index; cursor < digits_end; ++cursor) {
        const auto digit = static_cast<std::int64_t>(text[cursor] - '0');
        const auto scaled = checked_mul_add(whole, 10, digit);
        if (!scaled.has_value()) {
            return std::nullopt;
        }
        whole = *scaled;
    }
    std::size_t cursor = digits_end;
    std::int64_t fraction_nanos = 0;
    std::size_t fraction_digits = 0;
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            if (fraction_digits < 9) {
                fraction_nanos = fraction_nanos * 10 + (text[cursor] - '0');
                ++fraction_digits;
            } else {
                return std::nullopt;
            }
            ++cursor;
        }
        if (fraction_digits == 0) {
            return std::nullopt;
        }
        for (std::size_t pad = fraction_digits; pad < 9; ++pad) {
            fraction_nanos *= 10;
        }
    }
    const std::string_view unit = text.substr(cursor);
    std::int64_t unit_nanos = 0;
    if (unit == "ns") {
        unit_nanos = 1;
    } else if (unit == "us") {
        unit_nanos = 1000;
    } else if (unit == "ms") {
        unit_nanos = 1000 * 1000;
    } else if (unit == "s") {
        unit_nanos = 1000LL * 1000 * 1000;
    } else if (unit == "m") {
        unit_nanos = 60LL * 1000 * 1000 * 1000;
    } else if (unit == "h") {
        unit_nanos = 3600LL * 1000 * 1000 * 1000;
    } else if (unit == "d") {
        unit_nanos = 86400LL * 1000 * 1000 * 1000;
    } else {
        return std::nullopt;
    }
    const auto whole_nanos = checked_mul_add(whole, unit_nanos, 0);
    if (!whole_nanos.has_value()) {
        return std::nullopt;
    }
    const auto total = checked_add(*whole_nanos, fraction_nanos);
    if (!total.has_value()) {
        return std::nullopt;
    }
    if (negative) {
        if (*total == std::numeric_limits<std::int64_t>::min()) {
            return std::nullopt;
        }
        return Duration(-*total);
    }
    return Duration(*total);
}

Clock::~Clock() = default;

Instant SystemClock::now() const {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
    return Instant(static_cast<std::int64_t>(nanoseconds));
}

void ManualClock::advance(Duration delta) noexcept {
    const auto shifted_value = current_.shifted(delta);
    if (shifted_value.has_value()) {
        current_ = *shifted_value;
    }
}

}  // namespace asset_health
