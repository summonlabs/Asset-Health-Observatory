// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "asset_health/limits.hpp"

namespace asset_health {
namespace {

[[nodiscard]] std::string milli_text(std::int64_t milli) {
    const bool negative = milli < 0;
    const std::int64_t magnitude = negative ? -milli : milli;
    std::string text;
    if (negative && magnitude != 0) {
        text.push_back('-');
    }
    text.append(std::to_string(magnitude / 1000));
    text.push_back('.');
    std::string fraction = std::to_string(magnitude % 1000);
    fraction.insert(fraction.begin(), 3u - fraction.size(), '0');
    text.append(fraction);
    return text;
}

void append_optional(std::string& text, const char* label, const std::optional<std::int64_t>& value) {
    text.push_back(' ');
    text.append(label);
    text.push_back(' ');
    text.append(value.has_value() ? milli_text(*value) : std::string("-"));
}

/// FNV-1a over the canonical policy text. A non-cryptographic hash is the right
/// tool here: it detects a change of threshold, and an adversary who can edit the
/// policy is already past every check this observatory makes.
[[nodiscard]] std::string fingerprint_of(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kHex[(hash >> shift) & 0x0F]);
    }
    return out;
}

[[nodiscard]] bool thresholds_ordered(const MetricPolicy& policy) noexcept {
    if (policy.warn_high.has_value() && policy.critical_high.has_value() &&
        *policy.critical_high < *policy.warn_high) {
        return false;
    }
    if (policy.warn_low.has_value() && policy.critical_low.has_value() && *policy.critical_low > *policy.warn_low) {
        return false;
    }
    if (policy.warn_low.has_value() && policy.warn_high.has_value() && *policy.warn_low >= *policy.warn_high) {
        return false;
    }
    if (policy.critical_low.has_value() && policy.critical_high.has_value() &&
        *policy.critical_low >= *policy.critical_high) {
        return false;
    }
    return policy.conflict_absolute_milli >= 0 && policy.conflict_relative_ppm >= 0;
}

[[nodiscard]] HealthPolicy::Parts standard_parts() {
    HealthPolicy::Parts value;
    value.id = *Token::parse("standard-dccp-health");
    value.version = PolicyVersion(1);
    value.freshness.dynamic_window = Duration::from_seconds(300);
    value.freshness.static_window = Duration::from_days(30);
    value.freshness.future_tolerance = Duration::from_seconds(5);
    value.freshness.static_unaged = false;

    // The thresholds below are the standard policy's judgement about a
    // liquid-cooled accelerator facility. They are policy, not physics: a
    // deployment that knows better supplies its own policy, and the fingerprint
    // recorded with every assessment makes the change visible.
    auto set = [&value](MetricKind metric, const MetricPolicy& policy) {
        value.metrics[static_cast<std::size_t>(metric)] = policy;
    };

    MetricPolicy temperature;
    temperature.required = true;
    temperature.warn_low = 5000;        // 5 C
    temperature.warn_high = 75000;      // 75 C
    temperature.critical_high = 85000;  // 85 C
    temperature.conflict_absolute_milli = 2000;
    temperature.conflict_relative_ppm = 10000;
    set(MetricKind::Temperature, temperature);

    MetricPolicy power;
    power.warn_high = 1200000;  // 1200 W
    power.critical_high = 1500000;
    power.conflict_absolute_milli = 50000;
    power.conflict_relative_ppm = 20000;
    set(MetricKind::PowerDraw, power);

    MetricPolicy voltage;
    voltage.warn_low = 11000;
    voltage.warn_high = 13000;
    voltage.critical_low = 10500;
    voltage.critical_high = 13500;
    voltage.conflict_absolute_milli = 200;
    voltage.conflict_relative_ppm = 5000;
    set(MetricKind::Voltage, voltage);

    MetricPolicy current;
    current.warn_high = 100000;  // 100 A
    current.critical_high = 120000;
    current.conflict_absolute_milli = 2000;
    set(MetricKind::Current, current);

    MetricPolicy fan;
    fan.warn_low = 1500;  // rpm
    fan.critical_low = 500;
    fan.conflict_absolute_milli = 200;
    set(MetricKind::FanSpeed, fan);

    MetricPolicy pressure;
    pressure.warn_low = 50000;  // 0.5 bar
    pressure.warn_high = 400000;
    pressure.critical_low = 20000;
    pressure.critical_high = 600000;
    pressure.conflict_absolute_milli = 5000;
    set(MetricKind::Pressure, pressure);

    MetricPolicy flow;
    flow.warn_low = 2000;  // 2 l/min
    flow.critical_low = 500;
    flow.conflict_absolute_milli = 200;
    set(MetricKind::FlowRate, flow);

    MetricPolicy humidity;
    humidity.warn_low = 20000;
    humidity.warn_high = 70000;
    humidity.critical_low = 10000;
    humidity.critical_high = 85000;
    humidity.conflict_absolute_milli = 5000;
    set(MetricKind::Humidity, humidity);

    MetricPolicy utilization;
    utilization.warn_high = 95000;
    utilization.critical_high = 100000;
    utilization.conflict_absolute_milli = 10000;
    set(MetricKind::Utilization, utilization);

    MetricPolicy correctable;
    correctable.warn_high = 100;
    correctable.critical_high = 1000;
    correctable.conflict_absolute_milli = 100;
    set(MetricKind::EccCorrectable, correctable);

    MetricPolicy uncorrectable;
    uncorrectable.warn_high = 0;  // any uncorrectable error is a warning
    uncorrectable.critical_high = 10;
    uncorrectable.conflict_absolute_milli = 10;
    set(MetricKind::EccUncorrectable, uncorrectable);

    MetricPolicy link_errors;
    link_errors.warn_high = 1000;
    link_errors.critical_high = 100000;
    link_errors.conflict_absolute_milli = 1000;
    link_errors.conflict_relative_ppm = 100000;
    set(MetricKind::LinkErrors, link_errors);

    MetricPolicy redundancy;
    redundancy.warn_low = 2000;      // two spares wanted
    redundancy.critical_low = 1000;  // one spare is the floor
    redundancy.conflict_absolute_milli = 1000;
    set(MetricKind::PowerSupplyRedundancy, redundancy);

    MetricPolicy battery;
    battery.warn_low = 70000;
    battery.critical_low = 50000;
    battery.conflict_absolute_milli = 5000;
    set(MetricKind::BatteryHealth, battery);

    MetricPolicy uptime;
    uptime.conflict_absolute_milli = 3600000;  // one hour
    set(MetricKind::Uptime, uptime);

    MetricPolicy wear;
    wear.warn_high = 80000;
    wear.critical_high = 95000;
    wear.conflict_absolute_milli = 5000;
    set(MetricKind::Wear, wear);

    value.degradation.window = Duration::from_hours(24);
    value.degradation.min_samples = 3;
    value.degradation.min_delta_milli = 1000;
    value.degradation.max_samples = 32;

    value.maintenance.masking_enabled = true;
    value.risk.weight_age = Rational::from_integer(1);
    value.risk.weight_fault_severity = Rational::from_integer(3);
    value.risk.weight_fault_recurrence = Rational::from_integer(2);
    value.risk.weight_degradation = Rational::from_integer(2);
    value.risk.weight_threshold_excursion = Rational::from_integer(2);
    value.risk.weight_maintenance_burden = Rational::from_integer(1);
    value.risk.weight_firmware_unknown = Rational::from_integer(1);
    value.clock_skew_tolerance = Duration::from_seconds(5);
    return value;
}

}  // namespace

std::string HealthPolicy::to_string() const {
    std::string text;
    text.append("policy ");
    text.append(id_.str());
    text.append(" version ");
    text.append(version_.to_string());
    text.push_back('\n');
    text.append("freshness dynamic-window ");
    text.append(freshness_.dynamic_window.to_seconds_string());
    text.append(" static-window ");
    text.append(freshness_.static_window.to_seconds_string());
    text.append(" future-tolerance ");
    text.append(freshness_.future_tolerance.to_seconds_string());
    text.append(" static-unaged ");
    text.append(freshness_.static_unaged ? "true" : "false");
    text.push_back('\n');
    text.append("skew-tolerance ");
    text.append(clock_skew_tolerance_.to_seconds_string());
    text.push_back('\n');
    for (std::size_t index = 0; index < kMetricKindCount; ++index) {
        const MetricPolicy& metric = metrics_[index];
        text.append("metric ");
        text.append(asset_health::to_string(static_cast<MetricKind>(index)));
        text.append(" required ");
        text.append(metric.required ? "true" : "false");
        append_optional(text, "warn-low", metric.warn_low);
        append_optional(text, "warn-high", metric.warn_high);
        append_optional(text, "critical-low", metric.critical_low);
        append_optional(text, "critical-high", metric.critical_high);
        text.append(" conflict-absolute ");
        text.append(milli_text(metric.conflict_absolute_milli));
        text.append(" conflict-relative-ppm ");
        text.append(std::to_string(metric.conflict_relative_ppm));
        text.push_back('\n');
    }
    for (std::size_t index = 0; index < kFaultSeverityCount; ++index) {
        text.append("fault-impact ");
        text.append(asset_health::to_string(static_cast<FaultSeverity>(index)));
        text.push_back(' ');
        text.append(asset_health::to_string(fault_impact_[index]));
        text.push_back('\n');
    }
    text.append("warning-impact ");
    text.append(asset_health::to_string(warning_impact_));
    text.append(" critical-impact ");
    text.append(asset_health::to_string(critical_impact_));
    text.append(" degradation-impact ");
    text.append(asset_health::to_string(degradation_impact_));
    text.append(" firmware-behind-impact ");
    text.append(asset_health::to_string(firmware_behind_impact_));
    text.push_back('\n');
    text.append("uncertain-reading-impact ");
    text.append(asset_health::to_string(uncertain_reading_impact_));
    text.append(" conflict-impact ");
    text.append(asset_health::to_string(conflict_impact_));
    text.append(" cached-reading-impact ");
    text.append(asset_health::to_string(cached_reading_impact_));
    text.push_back('\n');
    text.append("unknown-lifecycle-cap ");
    text.append(asset_health::to_string(unknown_lifecycle_cap_));
    text.append(" missing-required-cap ");
    text.append(asset_health::to_string(missing_required_cap_));
    text.push_back('\n');
    text.append("degradation window ");
    text.append(degradation_.window.to_seconds_string());
    text.append(" min-samples ");
    text.append(std::to_string(degradation_.min_samples));
    text.append(" min-delta ");
    text.append(milli_text(degradation_.min_delta_milli));
    text.append(" max-samples ");
    text.append(std::to_string(degradation_.max_samples));
    text.push_back('\n');
    text.append("maintenance masking ");
    text.append(maintenance_.masking_enabled ? "true" : "false");
    text.append(" planned ");
    text.append(maintenance_.allow_planned ? "true" : "false");
    text.append(" corrective ");
    text.append(maintenance_.allow_corrective ? "true" : "false");
    text.append(" inspection ");
    text.append(maintenance_.allow_inspection ? "true" : "false");
    text.append(" calibration ");
    text.append(maintenance_.allow_calibration ? "true" : "false");
    text.append(" emergency ");
    text.append(maintenance_.allow_emergency ? "true" : "false");
    text.append(" require-active-window ");
    text.append(maintenance_.require_active_window ? "true" : "false");
    text.push_back('\n');
    text.append("risk weights age=");
    text.append(risk_.weight_age.to_string());
    text.append(" fault-severity=");
    text.append(risk_.weight_fault_severity.to_string());
    text.append(" fault-recurrence=");
    text.append(risk_.weight_fault_recurrence.to_string());
    text.append(" degradation=");
    text.append(risk_.weight_degradation.to_string());
    text.append(" excursion=");
    text.append(risk_.weight_threshold_excursion.to_string());
    text.append(" maintenance=");
    text.append(risk_.weight_maintenance_burden.to_string());
    text.append(" firmware=");
    text.append(risk_.weight_firmware_unknown.to_string());
    text.push_back('\n');
    text.append("risk saturation age-hours=");
    text.append(std::to_string(risk_.age_saturation_hours));
    text.append(" recurrence=");
    text.append(std::to_string(risk_.recurrence_saturation));
    text.append(" degradation=");
    text.append(std::to_string(risk_.degradation_saturation));
    text.append(" maintenance=");
    text.append(std::to_string(risk_.maintenance_saturation));
    text.append(" excursion=");
    text.append(std::to_string(risk_.excursion_saturation));
    text.push_back('\n');
    text.append("risk bands low=");
    text.append(risk_.band_low.to_string());
    text.append(" moderate=");
    text.append(risk_.band_moderate.to_string());
    text.append(" high=");
    text.append(risk_.band_high.to_string());
    text.push_back('\n');
    return text;
}

Outcome<HealthPolicy> HealthPolicy::make(Parts parts) {
    if (parts.id.empty()) {
        return Error(ErrorCode::InvalidPolicy, "a policy must carry an identifier");
    }
    if (parts.version.is_zero()) {
        return Error(ErrorCode::InvalidPolicy, "a policy version must be at least one");
    }
    if (parts.freshness.dynamic_window.is_negative() || parts.freshness.static_window.is_negative() ||
        parts.freshness.future_tolerance.is_negative()) {
        return Error(ErrorCode::InvalidPolicy, "freshness windows cannot be negative");
    }
    if (parts.clock_skew_tolerance.is_negative()) {
        return Error(ErrorCode::InvalidPolicy, "the clock skew tolerance cannot be negative");
    }
    for (std::size_t index = 0; index < kMetricKindCount; ++index) {
        if (!thresholds_ordered(parts.metrics[index])) {
            return Error(ErrorCode::InvalidPolicy, "metric thresholds are not ordered")
                .with_context("metric", std::string(asset_health::to_string(static_cast<MetricKind>(index))));
        }
    }
    const std::array<HealthState, 9> impacts{
        parts.warning_impact,          parts.critical_impact,         parts.degradation_impact,
        parts.uncertain_reading_impact, parts.conflict_impact,        parts.unknown_lifecycle_cap,
        parts.missing_required_cap,    parts.cached_reading_impact,   parts.firmware_behind_impact,
    };
    for (const HealthState impact : impacts) {
        if (static_cast<std::size_t>(impact) >= kHealthStateCount) {
            return Error(ErrorCode::InvalidPolicy, "a configured impact is not a canonical health state");
        }
    }
    for (const HealthState impact : parts.fault_impact) {
        if (static_cast<std::size_t>(impact) >= kHealthStateCount) {
            return Error(ErrorCode::InvalidPolicy, "a fault impact is not a canonical health state");
        }
    }
    if (parts.degradation.window <= Duration{}) {
        return Error(ErrorCode::InvalidPolicy, "the degradation window must be positive");
    }
    if (parts.degradation.min_samples < 2) {
        return Error(ErrorCode::InvalidPolicy, "a degradation indicator needs at least two samples to compare");
    }
    if (parts.degradation.max_samples < parts.degradation.min_samples) {
        return Error(ErrorCode::InvalidPolicy, "the degradation sample bound is below the minimum sample count");
    }
    if (parts.degradation.max_samples > limits::kMaxTrendSamples) {
        return Error(ErrorCode::InvalidPolicy, "the degradation sample bound exceeds the configured limit");
    }
    const std::array<Rational, 7> weights{
        parts.risk.weight_age,           parts.risk.weight_fault_severity,        parts.risk.weight_fault_recurrence,
        parts.risk.weight_degradation,   parts.risk.weight_threshold_excursion,   parts.risk.weight_maintenance_burden,
        parts.risk.weight_firmware_unknown,
    };
    for (const Rational& weight : weights) {
        if (weight.is_zero() || weight.is_negative()) {
            return Error(ErrorCode::InvalidPolicy, "every risk weight must be positive");
        }
    }
    if (parts.risk.age_saturation_hours < 1 || parts.risk.recurrence_saturation < 1 ||
        parts.risk.degradation_saturation < 1 || parts.risk.maintenance_saturation < 1 ||
        parts.risk.excursion_saturation < 1) {
        return Error(ErrorCode::InvalidPolicy, "every risk saturation bound must be at least one");
    }
    if (!(parts.risk.band_low < parts.risk.band_moderate) || !(parts.risk.band_moderate < parts.risk.band_high)) {
        return Error(ErrorCode::InvalidPolicy, "risk band boundaries must increase");
    }
    if (parts.risk.band_low.is_negative() || parts.risk.band_high > Rational::from_integer(1)) {
        return Error(ErrorCode::InvalidPolicy, "risk band boundaries must lie inside (0, 1]");
    }

    HealthPolicy policy;
    policy.id_ = std::move(parts.id);
    policy.version_ = parts.version;
    policy.freshness_ = parts.freshness;
    policy.metrics_ = parts.metrics;
    policy.fault_impact_ = parts.fault_impact;
    policy.warning_impact_ = parts.warning_impact;
    policy.critical_impact_ = parts.critical_impact;
    policy.degradation_impact_ = parts.degradation_impact;
    policy.firmware_behind_impact_ = parts.firmware_behind_impact;
    policy.uncertain_reading_impact_ = parts.uncertain_reading_impact;
    policy.conflict_impact_ = parts.conflict_impact;
    policy.unknown_lifecycle_cap_ = parts.unknown_lifecycle_cap;
    policy.missing_required_cap_ = parts.missing_required_cap;
    policy.cached_reading_impact_ = parts.cached_reading_impact;
    policy.degradation_ = parts.degradation;
    policy.maintenance_ = parts.maintenance;
    policy.risk_ = parts.risk;
    policy.clock_skew_tolerance_ = parts.clock_skew_tolerance;
    policy.fingerprint_ = fingerprint_of(policy.to_string());
    return policy;
}

const HealthPolicy& HealthPolicy::standard() {
    static const HealthPolicy policy = [] {
        const auto built = HealthPolicy::make(standard_parts());
        // The standard policy is a constant of this build. A failure to build it
        // is a defect in this file rather than a runtime condition, so it is
        // reported by producing an empty policy whose identifier says so; the
        // test suite asserts that the standard policy is valid and fingerprintable.
        return built.has_value() ? built.value() : HealthPolicy{};
    }();
    return policy;
}

bool HealthPolicy::masking_allowed(MaintenanceKind kind) const noexcept {
    if (!maintenance_.masking_enabled) {
        return false;
    }
    switch (kind) {
        case MaintenanceKind::Planned:
            return maintenance_.allow_planned;
        case MaintenanceKind::Corrective:
            return maintenance_.allow_corrective;
        case MaintenanceKind::Inspection:
            return maintenance_.allow_inspection;
        case MaintenanceKind::Calibration:
            return maintenance_.allow_calibration;
        case MaintenanceKind::Emergency:
            return maintenance_.allow_emergency;
    }
    return false;
}

}  // namespace asset_health
