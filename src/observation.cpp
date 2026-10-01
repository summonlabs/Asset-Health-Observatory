// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/observation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace asset_health {
namespace {

constexpr std::array<std::string_view, kUnitCount> kUnitNames{{
    "none", "c", "k", "w", "v", "a", "rpm", "count", "pct", "h", "s", "b", "pa", "lpm",
}};

constexpr std::array<std::string_view, kMetricKindCount> kMetricNames{{
    "temperature",
    "power-draw",
    "voltage",
    "current",
    "fan-speed",
    "pressure",
    "flow-rate",
    "humidity",
    "utilization",
    "ecc-correctable",
    "ecc-uncorrectable",
    "link-errors",
    "power-supply-redundancy",
    "battery-health",
    "uptime",
    "wear",
}};

constexpr std::array<std::string_view, kMetricKindCount> kMetricDescriptions{{
    "component or coolant temperature",
    "electrical power drawn by the asset",
    "supply or rail voltage",
    "current drawn by the asset",
    "rotational speed of a cooling fan",
    "coolant or gas pressure at the asset",
    "coolant flow rate through the asset",
    "relative humidity at the asset inlet",
    "fraction of capacity in use",
    "correctable memory error count",
    "uncorrectable memory error count",
    "link error count",
    "number of healthy power supplies beyond the minimum needed",
    "reported battery state of health",
    "hours the asset has been powered",
    "consumed endurance of a wear-limited part",
}};

constexpr std::array<std::string_view, kSampleQualityCount> kSampleQualityNames{{
    "good", "uncertain", "substituted", "bad",
}};

constexpr std::array<std::string_view, kFaultSeverityCount> kFaultSeverityNames{{
    "info", "warning", "minor", "major", "critical",
}};

constexpr std::array<std::string_view, kLifecycleStateCount> kLifecycleNames{{
    "ordered", "staged", "installed", "commissioning", "active", "degraded",
    "maintenance", "quarantined", "retiring", "retired", "replaced", "removed",
}};

constexpr std::array<std::string_view, kServiceClassCount> kServiceClassNames{{
    "pre-service", "in-service", "in-service-impaired", "service-withheld", "draining", "decommissioned", "disposed",
}};

constexpr std::array<std::string_view, kMaintenanceKindCount> kMaintenanceKindNames{{
    "planned", "corrective", "inspection", "calibration", "emergency",
}};

constexpr std::array<std::string_view, kMaintenanceStateCount> kMaintenanceStateNames{{
    "scheduled", "active", "completed", "cancelled",
}};

constexpr std::array<std::string_view, kMaskScopeCount> kMaskScopeNames{{
    "none", "telemetry", "faults", "telemetry-and-faults",
}};

constexpr std::array<std::string_view, kFirmwareComplianceCount> kFirmwareComplianceNames{{
    "unknown", "matches", "behind", "ahead",
}};

constexpr std::array<std::string_view, kEvidenceKindCount> kEvidenceKindNames{{
    "identity-statement", "lifecycle-statement", "maintenance-statement",
    "firmware-statement", "telemetry-reading", "fault-statement",
}};

[[nodiscard]] constexpr Unit unit_for(MetricKind metric) noexcept {
    switch (metric) {
        case MetricKind::Temperature:
            return Unit::Celsius;
        case MetricKind::PowerDraw:
            return Unit::Watts;
        case MetricKind::Voltage:
            return Unit::Volts;
        case MetricKind::Current:
            return Unit::Amperes;
        case MetricKind::FanSpeed:
            return Unit::Rpm;
        case MetricKind::Pressure:
            return Unit::Pascal;
        case MetricKind::FlowRate:
            return Unit::LitersPerMinute;
        case MetricKind::Humidity:
        case MetricKind::Utilization:
        case MetricKind::BatteryHealth:
        case MetricKind::Wear:
            return Unit::Percent;
        case MetricKind::EccCorrectable:
        case MetricKind::EccUncorrectable:
        case MetricKind::LinkErrors:
        case MetricKind::PowerSupplyRedundancy:
            return Unit::Count;
        case MetricKind::Uptime:
            return Unit::Hours;
    }
    return Unit::None;
}

/// Formats a milli-value as "<sign><whole>.<fraction>" with exactly three
/// fractional digits and no negative zero.
[[nodiscard]] std::string format_milli(std::int64_t milli) {
    const bool negative = milli < 0;
    const std::int64_t magnitude = negative ? -milli : milli;
    std::string text;
    if (negative && magnitude != 0) {
        text.push_back('-');
    }
    text.append(std::to_string(magnitude / kMilliScale));
    text.push_back('.');
    std::string fraction = std::to_string(magnitude % kMilliScale);
    fraction.insert(fraction.begin(), 3u - fraction.size(), '0');
    text.append(fraction);
    return text;
}

[[nodiscard]] bool parse_milli(std::string_view text, std::int64_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        ++index;
    }
    std::size_t integer_digits = 0;
    std::int64_t whole = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        const auto digit = static_cast<std::int64_t>(text[index] - '0');
        const auto scaled = checked_mul_add(whole, 10, digit);
        if (!scaled.has_value()) {
            return false;
        }
        whole = *scaled;
        ++integer_digits;
        ++index;
    }
    if (integer_digits == 0) {
        return false;
    }
    std::int64_t fraction = 0;
    std::size_t fraction_digits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (fraction_digits >= 3) {
                // A quantity carries thousandths. A producer that sends more
                // precision is asked to round it: this observatory does not
                // silently discard digits from a measurement.
                return false;
            }
            fraction = fraction * 10 + (text[index] - '0');
            ++fraction_digits;
            ++index;
        }
        if (fraction_digits == 0) {
            return false;
        }
        for (std::size_t pad = fraction_digits; pad < 3; ++pad) {
            fraction *= 10;
        }
    }
    if (index != text.size()) {
        return false;
    }
    const auto scaled_whole = checked_mul_add(whole, kMilliScale, 0);
    if (!scaled_whole.has_value()) {
        return false;
    }
    const auto total = checked_add(*scaled_whole, fraction);
    if (!total.has_value()) {
        return false;
    }
    if (negative) {
        if (*total == std::numeric_limits<std::int64_t>::min()) {
            return false;
        }
        out = -*total;
        return true;
    }
    out = *total;
    return true;
}

}  // namespace

std::string_view to_string(Unit unit) noexcept {
    const auto index = static_cast<std::size_t>(unit);
    return index < kUnitNames.size() ? kUnitNames[index] : std::string_view{"unknown"};
}

std::optional<Unit> parse_unit(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kUnitNames.size(); ++index) {
        if (kUnitNames[index] == text) {
            return static_cast<Unit>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(MetricKind metric) noexcept {
    const auto index = static_cast<std::size_t>(metric);
    return index < kMetricNames.size() ? kMetricNames[index] : std::string_view{"unknown"};
}

std::optional<MetricKind> parse_metric(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kMetricNames.size(); ++index) {
        if (kMetricNames[index] == text) {
            return static_cast<MetricKind>(index);
        }
    }
    return std::nullopt;
}

const char* metric_description(MetricKind metric) noexcept {
    const auto index = static_cast<std::size_t>(metric);
    return index < kMetricDescriptions.size() ? kMetricDescriptions[index].data() : "unknown metric";
}

Unit canonical_unit(MetricKind metric) noexcept { return unit_for(metric); }

AdverseDirection adverse_direction(MetricKind metric) noexcept {
    switch (metric) {
        case MetricKind::FanSpeed:
        case MetricKind::PowerSupplyRedundancy:
        case MetricKind::BatteryHealth:
            return AdverseDirection::Lower;
        case MetricKind::Voltage:
        case MetricKind::Pressure:
        case MetricKind::Humidity:
            return AdverseDirection::TwoSided;
        default:
            return AdverseDirection::Higher;
    }
}

MetricSemantics metric_semantics(MetricKind metric) noexcept {
    switch (metric) {
        case MetricKind::EccCorrectable:
        case MetricKind::EccUncorrectable:
        case MetricKind::LinkErrors:
        case MetricKind::Uptime:
            return MetricSemantics::Counter;
        default:
            return MetricSemantics::Gauge;
    }
}

std::string_view to_string(SampleQuality quality) noexcept {
    const auto index = static_cast<std::size_t>(quality);
    return index < kSampleQualityNames.size() ? kSampleQualityNames[index] : std::string_view{"unknown"};
}

std::optional<SampleQuality> parse_sample_quality(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kSampleQualityNames.size(); ++index) {
        if (kSampleQualityNames[index] == text) {
            return static_cast<SampleQuality>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(FaultSeverity severity) noexcept {
    const auto index = static_cast<std::size_t>(severity);
    return index < kFaultSeverityNames.size() ? kFaultSeverityNames[index] : std::string_view{"unknown"};
}

std::optional<FaultSeverity> parse_fault_severity(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kFaultSeverityNames.size(); ++index) {
        if (kFaultSeverityNames[index] == text) {
            return static_cast<FaultSeverity>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(FaultStatus status) noexcept {
    return status == FaultStatus::Active ? std::string_view{"active"} : std::string_view{"cleared"};
}

std::optional<FaultStatus> parse_fault_status(std::string_view text) noexcept {
    if (text == "active") {
        return FaultStatus::Active;
    }
    if (text == "cleared") {
        return FaultStatus::Cleared;
    }
    return std::nullopt;
}

std::string_view to_string(LifecycleState state) noexcept {
    const auto index = static_cast<std::size_t>(state);
    return index < kLifecycleNames.size() ? kLifecycleNames[index] : std::string_view{"unknown"};
}

std::string_view to_string(ServiceClass klass) noexcept {
    const auto index = static_cast<std::size_t>(klass);
    return index < kServiceClassNames.size() ? kServiceClassNames[index] : std::string_view{"unknown"};
}

std::optional<LifecycleState> parse_lifecycle_state(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kLifecycleNames.size(); ++index) {
        if (kLifecycleNames[index] == text) {
            return static_cast<LifecycleState>(index);
        }
    }
    return std::nullopt;
}

ServiceClass service_class(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Ordered:
        case LifecycleState::Staged:
        case LifecycleState::Installed:
        case LifecycleState::Commissioning:
            return ServiceClass::PreService;
        case LifecycleState::Active:
            return ServiceClass::InService;
        case LifecycleState::Degraded:
            return ServiceClass::InServiceImpaired;
        case LifecycleState::Maintenance:
        case LifecycleState::Quarantined:
            return ServiceClass::ServiceWithheld;
        case LifecycleState::Retiring:
            return ServiceClass::Draining;
        case LifecycleState::Retired:
        case LifecycleState::Replaced:
            return ServiceClass::Decommissioned;
        case LifecycleState::Removed:
            return ServiceClass::Disposed;
    }
    return ServiceClass::PreService;
}

bool is_in_service(LifecycleState state) noexcept {
    const ServiceClass klass = service_class(state);
    return klass == ServiceClass::InService || klass == ServiceClass::InServiceImpaired ||
           klass == ServiceClass::ServiceWithheld || klass == ServiceClass::Draining;
}

bool is_end_of_life(LifecycleState state) noexcept {
    const ServiceClass klass = service_class(state);
    return klass == ServiceClass::Decommissioned || klass == ServiceClass::Disposed;
}

std::string_view to_string(MaintenanceKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kMaintenanceKindNames.size() ? kMaintenanceKindNames[index] : std::string_view{"unknown"};
}

std::optional<MaintenanceKind> parse_maintenance_kind(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kMaintenanceKindNames.size(); ++index) {
        if (kMaintenanceKindNames[index] == text) {
            return static_cast<MaintenanceKind>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(MaintenanceState state) noexcept {
    const auto index = static_cast<std::size_t>(state);
    return index < kMaintenanceStateNames.size() ? kMaintenanceStateNames[index] : std::string_view{"unknown"};
}

std::optional<MaintenanceState> parse_maintenance_state(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kMaintenanceStateNames.size(); ++index) {
        if (kMaintenanceStateNames[index] == text) {
            return static_cast<MaintenanceState>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(MaskScope scope) noexcept {
    const auto index = static_cast<std::size_t>(scope);
    return index < kMaskScopeNames.size() ? kMaskScopeNames[index] : std::string_view{"unknown"};
}

std::optional<MaskScope> parse_mask_scope(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kMaskScopeNames.size(); ++index) {
        if (kMaskScopeNames[index] == text) {
            return static_cast<MaskScope>(index);
        }
    }
    return std::nullopt;
}

bool masks_telemetry(MaskScope scope) noexcept {
    return scope == MaskScope::Telemetry || scope == MaskScope::TelemetryAndFaults;
}

bool masks_faults(MaskScope scope) noexcept {
    return scope == MaskScope::Faults || scope == MaskScope::TelemetryAndFaults;
}

std::string_view to_string(FirmwareCompliance compliance) noexcept {
    const auto index = static_cast<std::size_t>(compliance);
    return index < kFirmwareComplianceNames.size() ? kFirmwareComplianceNames[index] : std::string_view{"unknown"};
}

std::optional<FirmwareCompliance> parse_firmware_compliance(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kFirmwareComplianceNames.size(); ++index) {
        if (kFirmwareComplianceNames[index] == text) {
            return static_cast<FirmwareCompliance>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(EvidenceKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kEvidenceKindNames.size() ? kEvidenceKindNames[index] : std::string_view{"unknown"};
}

std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kEvidenceKindNames.size(); ++index) {
        if (kEvidenceKindNames[index] == text) {
            return static_cast<EvidenceKind>(index);
        }
    }
    return std::nullopt;
}

AuthorityDomain owning_domain(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::IdentityStatement:
            return AuthorityDomain::Identity;
        case EvidenceKind::LifecycleStatement:
            return AuthorityDomain::Lifecycle;
        case EvidenceKind::MaintenanceStatement:
            return AuthorityDomain::Maintenance;
        case EvidenceKind::FirmwareStatement:
            return AuthorityDomain::Firmware;
        case EvidenceKind::TelemetryReading:
        case EvidenceKind::FaultStatement:
            return AuthorityDomain::Observation;
    }
    return AuthorityDomain::Observation;
}

FreshnessClass freshness_class_of(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::TelemetryReading:
        case EvidenceKind::FaultStatement:
            return FreshnessClass::Dynamic;
        case EvidenceKind::IdentityStatement:
        case EvidenceKind::LifecycleStatement:
        case EvidenceKind::MaintenanceStatement:
        case EvidenceKind::FirmwareStatement:
            return FreshnessClass::Static;
    }
    return FreshnessClass::Dynamic;
}

std::optional<Quantity> Quantity::make(Unit unit, std::int64_t milli) noexcept {
    Quantity value;
    value.unit_ = unit;
    value.milli_ = milli;
    return value;
}

std::optional<Quantity> Quantity::parse(std::string_view text) noexcept {
    const std::size_t space = text.find(' ');
    const std::string_view number_text = space == std::string_view::npos ? text.substr(0, text.size())
                                                                        : text.substr(0, space);
    std::string_view unit_text;
    if (space != std::string_view::npos) {
        unit_text = text.substr(space + 1);
        if (unit_text.empty() || unit_text.find(' ') != std::string_view::npos) {
            return std::nullopt;
        }
    } else {
        // Without a space, the unit is the trailing run of letters.
        std::size_t split = number_text.size();
        while (split > 0) {
            const char c = number_text[split - 1];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                --split;
                continue;
            }
            break;
        }
        if (split == number_text.size() || split == 0) {
            return std::nullopt;
        }
        unit_text = number_text.substr(split);
    }
    const std::string_view digits = space == std::string_view::npos ? number_text.substr(0, number_text.size() - unit_text.size())
                                                                    : number_text;
    const auto unit = parse_unit(unit_text);
    if (!unit.has_value()) {
        return std::nullopt;
    }
    std::int64_t milli = 0;
    if (!parse_milli(digits, milli)) {
        return std::nullopt;
    }
    return Quantity::make(*unit, milli);
}

std::optional<std::int64_t> Quantity::difference_milli(const Quantity& other) const noexcept {
    if (unit_ != other.unit_) {
        return std::nullopt;
    }
    return checked_add(milli_, -other.milli_);
}

std::int64_t Quantity::absolute_difference_milli(const Quantity& other) const noexcept {
    if (unit_ != other.unit_) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (milli_ >= other.milli_) {
        return milli_ - other.milli_;
    }
    if (other.milli_ == std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    const std::int64_t difference = other.milli_ - milli_;
    return difference;
}

std::string Quantity::to_string() const {
    std::string text = format_milli(milli_);
    text.push_back(' ');
    text.append(asset_health::to_string(unit_));
    return text;
}

std::optional<Error> validate_payload(const EvidencePayload& payload) noexcept {
    switch (payload.kind) {
        case EvidenceKind::IdentityStatement:
            if (payload.identity.revision.is_zero()) {
                return Error(ErrorCode::InconsistentPayload,
                             "an identity statement must name the revision it observed");
            }
            return std::nullopt;
        case EvidenceKind::LifecycleStatement:
            if (static_cast<std::size_t>(payload.lifecycle.state) >= kLifecycleStateCount) {
                return Error(ErrorCode::InconsistentPayload, "lifecycle state is not a canonical name");
            }
            return std::nullopt;
        case EvidenceKind::MaintenanceStatement: {
            if (payload.maintenance.window_end <= payload.maintenance.window_start) {
                return Error(ErrorCode::InvalidTimeOrder, "a maintenance window must end after it starts");
            }
            return std::nullopt;
        }
        case EvidenceKind::FirmwareStatement:
            if (payload.firmware.version.empty()) {
                return Error(ErrorCode::InconsistentPayload, "a firmware statement must carry a version");
            }
            return std::nullopt;
        case EvidenceKind::TelemetryReading: {
            const Unit expected = canonical_unit(payload.telemetry.metric);
            if (payload.telemetry.value.unit() != expected) {
                return Error(ErrorCode::UnitMismatch, "the reading's unit is not the metric's canonical unit")
                    .with_context("metric", std::string(to_string(payload.telemetry.metric)))
                    .with_context("expected", std::string(to_string(expected)))
                    .with_context("actual", std::string(to_string(payload.telemetry.value.unit())));
            }
            return std::nullopt;
        }
        case EvidenceKind::FaultStatement:
            if (payload.fault.component.empty()) {
                return Error(ErrorCode::InconsistentPayload, "a fault statement must name a component");
            }
            if (payload.fault.code.empty()) {
                return Error(ErrorCode::InconsistentPayload, "a fault statement must carry a fault code");
            }
            return std::nullopt;
    }
    return Error(ErrorCode::UnknownEvidenceKind, "the payload kind is not a canonical name");
}

}  // namespace asset_health
