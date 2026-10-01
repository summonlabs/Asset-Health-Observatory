// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The health policy: every threshold, window, weight and mapping the evaluator
// uses, in one versioned value.
//
// Nothing in the evaluator is a constant hidden in code. A policy is supplied to
// every evaluation, it is fingerprinted, and the fingerprint is recorded with
// every published assessment, so a change of threshold is visible in history
// rather than silently rewriting the past.

#ifndef ASSET_HEALTH_POLICY_HPP
#define ASSET_HEALTH_POLICY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "asset_health/assessment.hpp"
#include "asset_health/error.hpp"
#include "asset_health/freshness.hpp"
#include "asset_health/observation.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// Thresholds and conflict tolerance for one metric.
///
/// The four thresholds are exact milli-unit bounds, each optional: a metric that
/// has no meaningful bound on a side says so with an empty optional rather than
/// with a sentinel. The conflict tolerance is an exact integer test: two readings
/// of the same metric conflict when their absolute difference exceeds
/// max(absolute_milli, relative_ppm * max(|a|, |b|) / 1,000,000).
struct MetricPolicy {
    /// When true, a usable reading of this metric must be present for the asset
    /// to be reported Healthy. Absence makes the answer Unknown, never Healthy.
    bool required = false;
    /// Excursion past this is a warning. Empty means the metric has no bound on
    /// that side.
    std::optional<std::int64_t> warn_low;
    std::optional<std::int64_t> warn_high;
    std::optional<std::int64_t> critical_low;
    std::optional<std::int64_t> critical_high;
    /// Absolute conflict tolerance in thousandths.
    std::int64_t conflict_absolute_milli = 0;
    /// Relative conflict tolerance in parts per million of the larger reading.
    std::int64_t conflict_relative_ppm = 0;
};

/// How a worsening trend is detected from a bounded window of samples.
struct DegradationPolicy {
    /// Only samples inside this window contribute.
    Duration window = Duration::from_hours(24);
    /// Fewer usable samples than this yields no indicator at all, rather than an
    /// indicator derived from a single point.
    std::size_t min_samples = 3;
    /// Net adverse movement, in thousandths, required before a trend is called
    /// worsening or improving. Movement within this band is Stable.
    std::int64_t min_delta_milli = 0;
    /// Most samples of one metric that contribute to one indicator.
    std::size_t max_samples = 32;
};

/// Whether an active maintenance window may set findings aside, and which kinds
/// of window may do so. Emergency and corrective work is not a reason to stop
/// counting what is wrong, so the default permits masking only for planned work,
/// inspections and calibrations.
struct MaintenancePolicy {
    bool masking_enabled = true;
    bool allow_planned = true;
    bool allow_inspection = true;
    bool allow_calibration = true;
    bool allow_corrective = false;
    bool allow_emergency = false;
    /// When true, a maintenance observation masks only while its window covers
    /// the evaluation instant, so a window that has run out stops masking.
    bool require_active_window = true;
};

/// The replacement-risk model, stated as data.
///
/// Total = sum(weight_i * score_i) / sum(weight_i) over the inputs that were
/// available, where score_i = min(raw_i, saturation_i) / saturation_i as an exact
/// rational. Every input's raw value, saturation point, score, weight and
/// contribution is reported, so the total can be recomputed by hand.
struct RiskPolicy {
    Rational weight_age = Rational::from_integer(1);
    Rational weight_fault_severity = Rational::from_integer(3);
    Rational weight_fault_recurrence = Rational::from_integer(2);
    Rational weight_degradation = Rational::from_integer(2);
    Rational weight_threshold_excursion = Rational::from_integer(2);
    Rational weight_maintenance_burden = Rational::from_integer(1);
    Rational weight_firmware_unknown = Rational::from_integer(1);
    /// Age in hours at which the age input saturates.
    std::int64_t age_saturation_hours = 87600;  // ten years
    /// Number of distinct fault statements at which recurrence saturates.
    std::int64_t recurrence_saturation = 8;
    /// Number of worsening degradation indicators at which that input saturates.
    std::int64_t degradation_saturation = 3;
    /// Number of corrective maintenance observations in the window at which the
    /// burden input saturates.
    std::int64_t maintenance_saturation = 4;
    /// Number of metrics in threshold excursion at which that input saturates.
    std::int64_t excursion_saturation = 3;
    /// Band boundaries on the total, each compared with total < boundary.
    Rational band_low = Rational::make(1, 4).value();
    Rational band_moderate = Rational::make(1, 2).value();
    Rational band_high = Rational::make(3, 4).value();
};

/// Everything the evaluator is allowed to depend on.
class ASSET_HEALTH_API HealthPolicy {
public:
    struct Parts {
        Token id;
        PolicyVersion version;
        FreshnessPolicy freshness{};
        std::array<MetricPolicy, kMetricKindCount> metrics{};
        /// Health state an active fault of each severity supports.
        std::array<HealthState, kFaultSeverityCount> fault_impact{
            HealthState::Healthy,   // Info
            HealthState::Degraded,  // Warning
            HealthState::Degraded,  // Minor
            HealthState::Critical,  // Major
            HealthState::Failed,    // Critical
        };
        /// Health state a warning-class threshold excursion supports.
        HealthState warning_impact = HealthState::Degraded;
        /// Health state a critical-class threshold excursion supports.
        HealthState critical_impact = HealthState::Critical;
        /// Health state a worsening degradation indicator supports.
        HealthState degradation_impact = HealthState::Degraded;
        /// Health state a firmware-below-baseline statement supports.
        HealthState firmware_behind_impact = HealthState::Healthy;
        /// Largest state an uncertain reading can support.
        HealthState uncertain_reading_impact = HealthState::Unknown;
        /// Smallest state a conflict supports when neither reading is in
        /// excursion. Never Healthy by default: two instruments that disagree
        /// have not established that the asset is well.
        HealthState conflict_impact = HealthState::Unknown;
        /// Largest state an assessment may report when the lifecycle state is
        /// not known.
        HealthState unknown_lifecycle_cap = HealthState::Unknown;
        /// Largest state an assessment may report when a required metric has no
        /// usable reading.
        HealthState missing_required_cap = HealthState::Unknown;
        /// Largest state a reading the producer flagged as cached can support.
        HealthState cached_reading_impact = HealthState::Unknown;
        DegradationPolicy degradation{};
        MaintenancePolicy maintenance{};
        RiskPolicy risk{};
        /// How far a source's observed_at may run ahead of received_at before the
        /// statement is refused as future-dated.
        Duration clock_skew_tolerance = Duration::from_seconds(5);
    };

    /// Validates and builds a policy. Rejects a zero risk weight, a metric whose
    /// thresholds are not ordered (warning inside critical), a saturation bound
    /// below one, a band sequence that is not strictly increasing, a degradation
    /// window that is not positive, and a minimum sample count below two.
    [[nodiscard]] static Outcome<HealthPolicy> make(Parts parts);

    /// The standard policy, documented threshold by threshold in the README.
    [[nodiscard]] static const HealthPolicy& standard();

    [[nodiscard]] const Token& id() const noexcept { return id_; }
    [[nodiscard]] PolicyVersion version() const noexcept { return version_; }
    [[nodiscard]] const FreshnessPolicy& freshness() const noexcept { return freshness_; }
    [[nodiscard]] const MetricPolicy& metric(MetricKind kind) const noexcept {
        return metrics_[static_cast<std::size_t>(kind)];
    }
    [[nodiscard]] const std::array<MetricPolicy, kMetricKindCount>& metrics() const noexcept { return metrics_; }
    [[nodiscard]] HealthState fault_impact(FaultSeverity severity) const noexcept {
        return fault_impact_[static_cast<std::size_t>(severity)];
    }
    [[nodiscard]] const std::array<HealthState, kFaultSeverityCount>& fault_impacts() const noexcept {
        return fault_impact_;
    }
    [[nodiscard]] HealthState warning_impact() const noexcept { return warning_impact_; }
    [[nodiscard]] HealthState critical_impact() const noexcept { return critical_impact_; }
    [[nodiscard]] HealthState degradation_impact() const noexcept { return degradation_impact_; }
    [[nodiscard]] HealthState firmware_behind_impact() const noexcept { return firmware_behind_impact_; }
    [[nodiscard]] HealthState uncertain_reading_impact() const noexcept { return uncertain_reading_impact_; }
    [[nodiscard]] HealthState conflict_impact() const noexcept { return conflict_impact_; }
    [[nodiscard]] HealthState unknown_lifecycle_cap() const noexcept { return unknown_lifecycle_cap_; }
    [[nodiscard]] HealthState missing_required_cap() const noexcept { return missing_required_cap_; }
    [[nodiscard]] HealthState cached_reading_impact() const noexcept { return cached_reading_impact_; }
    [[nodiscard]] const DegradationPolicy& degradation() const noexcept { return degradation_; }
    [[nodiscard]] const MaintenancePolicy& maintenance() const noexcept { return maintenance_; }
    [[nodiscard]] const RiskPolicy& risk() const noexcept { return risk_; }
    [[nodiscard]] Duration clock_skew_tolerance() const noexcept { return clock_skew_tolerance_; }

    /// True when a maintenance window of this kind may set findings aside.
    [[nodiscard]] bool masking_allowed(MaintenanceKind kind) const noexcept;

    /// Stable lowercase hexadecimal fingerprint of the policy's canonical form.
    /// Two policies that differ in any value produce different fingerprints.
    [[nodiscard]] const std::string& fingerprint() const noexcept { return fingerprint_; }

    /// Canonical multi-line rendering of every value in the policy. The
    /// fingerprint is computed over exactly this text, and the command line
    /// prints it for the policy subcommand.
    [[nodiscard]] std::string to_string() const;

private:
    HealthPolicy() = default;

    Token id_{};
    PolicyVersion version_{};
    FreshnessPolicy freshness_{};
    std::array<MetricPolicy, kMetricKindCount> metrics_{};
    std::array<HealthState, kFaultSeverityCount> fault_impact_{};
    HealthState warning_impact_ = HealthState::Degraded;
    HealthState critical_impact_ = HealthState::Critical;
    HealthState degradation_impact_ = HealthState::Degraded;
    HealthState firmware_behind_impact_ = HealthState::Healthy;
    HealthState uncertain_reading_impact_ = HealthState::Unknown;
    HealthState conflict_impact_ = HealthState::Unknown;
    HealthState unknown_lifecycle_cap_ = HealthState::Unknown;
    HealthState missing_required_cap_ = HealthState::Unknown;
    HealthState cached_reading_impact_ = HealthState::Unknown;
    DegradationPolicy degradation_{};
    MaintenancePolicy maintenance_{};
    RiskPolicy risk_{};
    Duration clock_skew_tolerance_ = Duration::from_seconds(5);
    std::string fingerprint_;
};

}  // namespace asset_health

#endif  // ASSET_HEALTH_POLICY_HPP
