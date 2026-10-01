// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The observation vocabulary: what a producer may say about an asset, in the
// canonical spelling this observatory stores and compares.
//
// Quantities are fixed point. A measurement is carried as an exact count of
// thousandths of its unit in a 64-bit integer, never as a binary floating point
// number, so two runs on two machines compare, threshold and total identically,
// and a conflict tolerance is an exact integer test rather than an epsilon.

#ifndef ASSET_HEALTH_OBSERVATION_HPP
#define ASSET_HEALTH_OBSERVATION_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "asset_health/error.hpp"
#include "asset_health/identity.hpp"
#include "asset_health/provenance.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// One thousandth of a unit. Every Quantity is an exact multiple of this.
inline constexpr std::int64_t kMilliScale = 1000;

/// Physical unit of a measurement. The unit is part of the value: a bare number
/// is not a measurement, and a quantity whose unit does not match the metric it
/// claims to measure is refused at admission.
enum class Unit : std::uint8_t {
    None = 0,
    Celsius = 1,
    Kelvin = 2,
    Watts = 3,
    Volts = 4,
    Amperes = 5,
    Rpm = 6,
    Count = 7,
    Percent = 8,
    Hours = 9,
    Seconds = 10,
    Bytes = 11,
    Pascal = 12,
    LitersPerMinute = 13,
};

/// Number of Unit enumerators.
inline constexpr std::size_t kUnitCount = 14;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(Unit unit) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<Unit> parse_unit(std::string_view text) noexcept;

/// A measurement: a unit and an exact count of thousandths.
class ASSET_HEALTH_API Quantity {
public:
    Quantity() = default;

    [[nodiscard]] static std::optional<Quantity> make(Unit unit, std::int64_t milli) noexcept;

    /// Parses "42.5 c", "42.5c" or "-3 rpm": an optional sign, digits, an optional
    /// fractional part of at most nine digits, and a canonical unit token. More
    /// precision than a thousandth is rejected rather than rounded, because
    /// silently rounding a measurement is silently changing it.
    [[nodiscard]] static std::optional<Quantity> parse(std::string_view text) noexcept;

    [[nodiscard]] Unit unit() const noexcept { return unit_; }
    [[nodiscard]] std::int64_t milli() const noexcept { return milli_; }
    [[nodiscard]] bool is_negative() const noexcept { return milli_ < 0; }

    /// The whole part, truncated toward zero.
    [[nodiscard]] std::int64_t whole() const noexcept { return milli_ / kMilliScale; }

    /// Exact difference in thousandths, or std::nullopt on overflow.
    [[nodiscard]] std::optional<std::int64_t> difference_milli(const Quantity& other) const noexcept;

    /// Exact absolute difference in thousandths, saturating at the maximum.
    [[nodiscard]] std::int64_t absolute_difference_milli(const Quantity& other) const noexcept;

    /// Canonical spelling: "<decimal> <unit>", for example "42.500 c".
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend bool operator==(const Quantity& lhs, const Quantity& rhs) noexcept {
        return lhs.unit_ == rhs.unit_ && lhs.milli_ == rhs.milli_;
    }
    [[nodiscard]] friend bool operator!=(const Quantity& lhs, const Quantity& rhs) noexcept { return !(lhs == rhs); }
    [[nodiscard]] friend bool operator<(const Quantity& lhs, const Quantity& rhs) noexcept {
        if (lhs.unit_ != rhs.unit_) {
            return lhs.unit_ < rhs.unit_;
        }
        return lhs.milli_ < rhs.milli_;
    }

private:
    Unit unit_ = Unit::None;
    std::int64_t milli_ = 0;
};

/// A measured property of an asset. The set is closed: a producer that needs a
/// property outside it must model it as the closest listed property and say so,
/// because an open namespace of metric names cannot be thresholded or compared.
enum class MetricKind : std::uint8_t {
    Temperature = 0,
    PowerDraw = 1,
    Voltage = 2,
    Current = 3,
    FanSpeed = 4,
    Pressure = 5,
    FlowRate = 6,
    Humidity = 7,
    Utilization = 8,
    EccCorrectable = 9,
    EccUncorrectable = 10,
    LinkErrors = 11,
    PowerSupplyRedundancy = 12,
    BatteryHealth = 13,
    Uptime = 14,
    Wear = 15,
};

/// Number of MetricKind enumerators.
inline constexpr std::size_t kMetricKindCount = 16;

/// Which direction of change is adverse for a metric. Used by degradation
/// detection and by threshold interpretation; it is data, not a comment, so a
/// new metric cannot be added without stating it.
enum class AdverseDirection : std::uint8_t {
    /// Larger values are worse (temperature, error counts, wear).
    Higher = 0,
    /// Smaller values are worse (fan speed, battery health, redundancy margin).
    Lower = 1,
    /// Both ends are adverse (voltage, pressure, humidity).
    TwoSided = 2,
};

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(MetricKind metric) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<MetricKind> parse_metric(std::string_view text) noexcept;
[[nodiscard]] ASSET_HEALTH_API const char* metric_description(MetricKind metric) noexcept;

/// The unit every observation of this metric must carry.
[[nodiscard]] ASSET_HEALTH_API Unit canonical_unit(MetricKind metric) noexcept;

/// The direction of change that is adverse for this metric.
[[nodiscard]] ASSET_HEALTH_API AdverseDirection adverse_direction(MetricKind metric) noexcept;

/// Whether a metric's value can be summed, averaged or totalled across samples.
/// A counter (error counts, uptime) is a running total whose differences are
/// meaningful; a gauge is a reading whose value is meaningful directly. Trend
/// analysis treats the two differently, and saying which is which here keeps
/// that decision out of the analysis code.
enum class MetricSemantics : std::uint8_t { Gauge = 0, Counter = 1 };

[[nodiscard]] ASSET_HEALTH_API MetricSemantics metric_semantics(MetricKind metric) noexcept;

/// The quality the producer attached to a reading. This is the producer's own
/// assessment of its instrument, carried through unchanged: an Uncertain reading
/// is never promoted to Good because it is the only reading available.
enum class SampleQuality : std::uint8_t {
    Good = 0,
    Uncertain = 1,
    Substituted = 2,
    Bad = 3,
};

/// Number of SampleQuality enumerators.
inline constexpr std::size_t kSampleQualityCount = 4;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(SampleQuality quality) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<SampleQuality> parse_sample_quality(std::string_view text) noexcept;

/// How bad an observed condition is, in the vocabulary fault producers already
/// use. The mapping from this to a health state is policy data, not a constant
/// hidden in the evaluator.
enum class FaultSeverity : std::uint8_t {
    Info = 0,
    Warning = 1,
    Minor = 2,
    Major = 3,
    Critical = 4,
};

/// Number of FaultSeverity enumerators.
inline constexpr std::size_t kFaultSeverityCount = 5;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(FaultSeverity severity) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<FaultSeverity> parse_fault_severity(std::string_view text) noexcept;

/// Whether a fault stands or has been cleared. A cleared fault is history: it
/// contributes to recurrence, never to current severity.
enum class FaultStatus : std::uint8_t { Active = 0, Cleared = 1 };

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(FaultStatus status) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<FaultStatus> parse_fault_status(std::string_view text) noexcept;

/// Canonical fault code. Lowercase token, as produced after the producer has
/// mapped its own spelling onto the contract.
struct FaultCodeTag;
using FaultCode = TaggedToken<FaultCodeTag>;

/// A lifecycle state as published by the hardware lifecycle authority. The names
/// are that authority's canonical names and are stored as names, not ordinals,
/// so a renumbering there cannot silently reinterpret this store's history.
///
/// This observatory never enters a state, never transitions one, and never
/// writes one back. It records what it was told, when it was told.
enum class LifecycleState : std::uint8_t {
    Ordered = 0,
    Staged = 1,
    Installed = 2,
    Commissioning = 3,
    Active = 4,
    Degraded = 5,
    Maintenance = 6,
    Quarantined = 7,
    Retiring = 8,
    Retired = 9,
    Replaced = 10,
    Removed = 11,
};

/// Number of LifecycleState enumerators.
inline constexpr std::size_t kLifecycleStateCount = 12;

/// Coarse grouping of a lifecycle state, used for service gating only. Two
/// states in the same class remain distinct states.
enum class ServiceClass : std::uint8_t {
    /// No service yet: the object exists and is not in service.
    PreService = 0,
    /// In service and nothing has been acknowledged as wrong.
    InService = 1,
    /// In service with an acknowledged impairment.
    InServiceImpaired = 2,
    /// Withdrawn from service on purpose, or withheld pending a decision.
    ServiceWithheld = 3,
    /// Being drained ahead of decommissioning.
    Draining = 4,
    /// Decommissioned but still physically present.
    Decommissioned = 5,
    /// Physically gone.
    Disposed = 6,
};

/// Number of ServiceClass enumerators.
inline constexpr std::size_t kServiceClassCount = 7;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(LifecycleState state) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(ServiceClass klass) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<LifecycleState> parse_lifecycle_state(std::string_view text) noexcept;
[[nodiscard]] ASSET_HEALTH_API ServiceClass service_class(LifecycleState state) noexcept;

/// True when the object is expected to be doing useful work in this state.
[[nodiscard]] ASSET_HEALTH_API bool is_in_service(LifecycleState state) noexcept;

/// True when the state says the object will not return to service.
[[nodiscard]] ASSET_HEALTH_API bool is_end_of_life(LifecycleState state) noexcept;

/// The kind of maintenance being described, as published by the maintenance
/// authority.
enum class MaintenanceKind : std::uint8_t {
    Planned = 0,
    Corrective = 1,
    Inspection = 2,
    Calibration = 3,
    Emergency = 4,
};

/// Number of MaintenanceKind enumerators.
inline constexpr std::size_t kMaintenanceKindCount = 5;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(MaintenanceKind kind) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<MaintenanceKind> parse_maintenance_kind(std::string_view text) noexcept;

/// Where the maintenance activity stands.
enum class MaintenanceState : std::uint8_t { Scheduled = 0, Active = 1, Completed = 2, Cancelled = 3 };

/// Number of MaintenanceState enumerators.
inline constexpr std::size_t kMaintenanceStateCount = 4;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(MaintenanceState state) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<MaintenanceState> parse_maintenance_state(std::string_view text) noexcept;

/// Which classes of evidence an active maintenance window asks this observatory
/// to set aside while judging health. The request is recorded, never obeyed
/// silently: a masked finding stays in the assessment, marked as masked, and the
/// window that masked it is named.
enum class MaskScope : std::uint8_t {
    None = 0,
    /// Threshold excursions are set aside; faults still count.
    Telemetry = 1,
    /// Fault severities are set aside; threshold excursions still count.
    Faults = 2,
    /// Both are set aside.
    TelemetryAndFaults = 3,
};

/// Number of MaskScope enumerators.
inline constexpr std::size_t kMaskScopeCount = 4;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(MaskScope scope) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<MaskScope> parse_mask_scope(std::string_view text) noexcept;

/// True when \p scope asks for telemetry excursions to be set aside.
[[nodiscard]] ASSET_HEALTH_API bool masks_telemetry(MaskScope scope) noexcept;

/// True when \p scope asks for fault severities to be set aside.
[[nodiscard]] ASSET_HEALTH_API bool masks_faults(MaskScope scope) noexcept;

/// The relationship between an observed firmware version and the baseline the
/// firmware authority published. This observatory observes the comparison; it
/// does not perform it, and Unknown is a first-class answer.
enum class FirmwareCompliance : std::uint8_t {
    Unknown = 0,
    Matches = 1,
    Behind = 2,
    Ahead = 3,
};

/// Number of FirmwareCompliance enumerators.
inline constexpr std::size_t kFirmwareComplianceCount = 4;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(FirmwareCompliance compliance) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<FirmwareCompliance> parse_firmware_compliance(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------

/// What the identity authority said about an asset when it was observed.
struct IdentityObservation {
    AssetRevision revision{};
    /// Producer-visible serial identity, if the producer published one.
    std::optional<DetailText> serial;
    /// True when the authority reports this generation as the current one for
    /// the identity.
    bool current = true;
};

/// One reading of one metric.
struct TelemetrySample {
    MetricKind metric = MetricKind::Temperature;
    Quantity value{};
    SampleQuality quality = SampleQuality::Good;
    /// True when the producer says the reading was taken from a cached or
    /// last-known value rather than read now.
    bool cached = false;
};

/// One fault statement about one component.
struct FaultObservation {
    ComponentId component{};
    FaultCode code{};
    FaultSeverity severity = FaultSeverity::Info;
    FaultStatus status = FaultStatus::Active;
    /// When the producer first saw the condition, if it says. Used for history
    /// only; ordering between two statements is decided by observation time and
    /// stream sequence.
    std::optional<Instant> first_seen;
    std::optional<DetailText> description;
};

/// One lifecycle state statement from the lifecycle authority.
struct LifecycleObservation {
    LifecycleState state = LifecycleState::Active;
    /// When the state took effect, as the authority reports it.
    std::optional<Instant> effective_at;
};

/// One maintenance statement from the maintenance authority.
struct MaintenanceObservation {
    MaintenanceKind kind = MaintenanceKind::Planned;
    MaintenanceState state = MaintenanceState::Scheduled;
    Instant window_start{};
    Instant window_end{};
    MaskScope mask = MaskScope::None;
    /// Maintenance ticket or work-order reference.
    std::optional<DetailText> reference;
};

/// One firmware statement from the firmware authority.
struct FirmwareObservation {
    ComponentId component{};
    VersionText version{};
    std::optional<VersionText> baseline;
    FirmwareCompliance compliance = FirmwareCompliance::Unknown;
};

/// Which domain owns a kind of evidence. Admission compares this with the source
/// kind's domain.
enum class EvidenceKind : std::uint8_t {
    IdentityStatement = 0,
    LifecycleStatement = 1,
    MaintenanceStatement = 2,
    FirmwareStatement = 3,
    TelemetryReading = 4,
    FaultStatement = 5,
};

/// Number of EvidenceKind enumerators.
inline constexpr std::size_t kEvidenceKindCount = 6;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept;

/// The authority domain that owns this kind of evidence. A source may submit a
/// kind only when its own domain equals this one.
[[nodiscard]] ASSET_HEALTH_API AuthorityDomain owning_domain(EvidenceKind kind) noexcept;

/// How an evidence kind ages. Dynamic evidence describes a moment and stops
/// being current when it is old or when the process that produced it restarted.
/// Static evidence describes a state that persists until it is superseded.
enum class FreshnessClass : std::uint8_t { Dynamic = 0, Static = 1 };

[[nodiscard]] ASSET_HEALTH_API FreshnessClass freshness_class_of(EvidenceKind kind) noexcept;

/// The union of every payload this observatory admits.
struct EvidencePayload {
    EvidenceKind kind = EvidenceKind::TelemetryReading;
    IdentityObservation identity{};
    LifecycleObservation lifecycle{};
    MaintenanceObservation maintenance{};
    FirmwareObservation firmware{};
    TelemetrySample telemetry{};
    FaultObservation fault{};
};

/// Validates a payload against its own kind: the unit matches the metric, the
/// maintenance window is ordered, the firmware statement carries a version, and
/// so on. Returns the reason a payload cannot be admitted.
[[nodiscard]] ASSET_HEALTH_API std::optional<Error> validate_payload(const EvidencePayload& payload) noexcept;

}  // namespace asset_health

#endif  // ASSET_HEALTH_OBSERVATION_HPP
