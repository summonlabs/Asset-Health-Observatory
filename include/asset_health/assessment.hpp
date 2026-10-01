// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// What this observatory concludes, and why.
//
// An assessment is a state, an ordered list of findings, the degradation
// indicators it derived, the exact evidence it depended on with that evidence's
// freshness, and an explicitly justified replacement-risk view. There is no
// opaque score anywhere in this file: a number that a reader cannot recompute
// from the listed inputs does not belong in an explanation.

#ifndef ASSET_HEALTH_ASSESSMENT_HPP
#define ASSET_HEALTH_ASSESSMENT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_health/error.hpp"
#include "asset_health/evidence.hpp"
#include "asset_health/freshness.hpp"
#include "asset_health/identity.hpp"
#include "asset_health/observation.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// The health state the evidence supports.
///
/// The ordering of the enumerators is the aggregation order, and it is part of
/// the contract: Healthy is better than Unknown, Unknown is better than
/// Degraded, and so on. Unknown sits between Healthy and Degraded on purpose —
/// missing evidence may not be reported as healthy, and it may not be reported
/// as degraded either, because nothing observed is wrong.
enum class HealthState : std::uint8_t {
    Healthy = 0,
    Unknown = 1,
    Degraded = 2,
    Critical = 3,
    Failed = 4,
    /// Terminal and orthogonal: a retired asset has no service health to judge.
    Retired = 5,
};

/// Number of HealthState enumerators.
inline constexpr std::size_t kHealthStateCount = 6;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(HealthState state) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<HealthState> parse_health_state(std::string_view text) noexcept;
[[nodiscard]] ASSET_HEALTH_API const char* health_state_description(HealthState state) noexcept;

/// Position in the aggregation order. Retired sorts last because it is terminal,
/// not because it is worse.
[[nodiscard]] ASSET_HEALTH_API std::uint8_t health_state_rank(HealthState state) noexcept;

/// The worse of two states in the aggregation order.
[[nodiscard]] ASSET_HEALTH_API HealthState worse_of(HealthState lhs, HealthState rhs) noexcept;

/// Reporting severity of a finding. This orders the explanation; the state a
/// finding supports is carried separately as its impact, so a finding can be
/// severe to read and still not move the state, and the assessment says which.
enum class FindingSeverity : std::uint8_t {
    Info = 0,
    Warning = 1,
    Minor = 2,
    Major = 3,
    Critical = 4,
};

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(FindingSeverity severity) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<FindingSeverity> parse_finding_severity(std::string_view text) noexcept;

/// What the assessment did with a finding.
enum class FindingDisposition : std::uint8_t {
    /// The finding stands and moved the state to at least its impact.
    Supported = 0,
    /// An active maintenance window asked for this class of evidence to be set
    /// aside. The finding is still reported, with the window that masked it.
    Masked = 1,
    /// The statement was about an earlier incarnation of the identity, so it is
    /// preserved and excluded from this assessment.
    Superseded = 2,
    /// The statement was refused: it claimed an authority it does not hold, or
    /// failed validation. Reported so the refusal is visible.
    Refused = 3,
    /// The evidence is present but not good enough to conclude from: an uncertain
    /// or substituted reading, a conflict, or a stale statement. Recorded so the
    /// gap in the explanation is visible.
    Uncertain = 4,
    /// Context that does not move the state: a matched firmware baseline, a
    /// completed maintenance window.
    Informational = 5,
};

/// Number of FindingDisposition enumerators.
inline constexpr std::size_t kFindingDispositionCount = 6;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(FindingDisposition disposition) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<FindingDisposition> parse_finding_disposition(std::string_view text) noexcept;

/// Every reason this observatory can put in a finding. The numeric values are
/// durable contract: they are persisted in history and exported, so a code is
/// appended, never renumbered, and never reused.
enum class FindingCode : std::uint16_t {
    None = 0,

    // --- Identity and generation (1..19) ----------------------------------
    IdentityObserved = 1,
    IdentityNotObserved = 2,
    IdentityReplaced = 3,
    GenerationSuperseded = 4,
    IdentityRevisionRegressed = 5,
    MultipleIdentityClaims = 6,

    // --- Lifecycle gating (20..39) ----------------------------------------
    LifecycleNotObserved = 20,
    LifecycleObserved = 21,
    AssetInService = 22,
    AssetNotYetInService = 23,
    AssetServiceWithheld = 24,
    AssetEndOfLife = 25,
    AssetDraining = 26,
    LifecycleStale = 27,
    LifecycleConflicting = 28,
    AssetServiceImpaired = 29,

    // --- Maintenance (40..59) ---------------------------------------------
    MaintenanceNotObserved = 40,
    MaintenanceScheduledNotStarted = 41,
    MaintenanceWindowActive = 42,
    MaintenanceWindowEnded = 43,
    MaintenanceCompletedInWindow = 44,
    MaskingApplied = 45,
    MaskingRefused = 46,
    CorrectiveMaintenanceHistory = 47,

    // --- Firmware and baseline (60..79) -----------------------------------
    FirmwareNotObserved = 60,
    FirmwareMatchesBaseline = 61,
    FirmwareBehindBaseline = 62,
    FirmwareAheadOfBaseline = 63,
    FirmwareBaselineUnknown = 64,
    FirmwareStale = 65,

    // --- Faults (80..109) -------------------------------------------------
    FaultActive = 80,
    FaultCleared = 81,
    FaultDuplicateSuppressed = 82,
    FaultOutOfOrderSuperseded = 83,
    FaultDescriptionUnavailable = 84,
    FaultRecurrence = 85,
    /// An active fault whose statement is no longer fresh: the last thing heard
    /// was that the fault stood, and nothing has confirmed it since.
    FaultStale = 86,
    /// An active fault statement recovered from an earlier store epoch.
    FaultRecovered = 87,
    /// Two sources of the same class disagree about one fault's state.
    FaultConflicting = 88,

    // --- Telemetry and thresholds (110..149) ------------------------------
    MetricWithinThreshold = 110,
    MetricWarningThreshold = 111,
    MetricCriticalThreshold = 112,
    MetricSubstituted = 113,
    MetricUncertain = 114,
    MetricBadQuality = 115,
    MetricMissing = 116,
    MetricStale = 117,
    MetricFutureDated = 118,
    MetricRecovered = 119,
    MetricSourceEpochSuperseded = 120,
    MetricConflicting = 121,
    MetricUnknownMetric = 122,
    MetricCached = 123,

    // --- Degradation (150..169) -------------------------------------------
    DegradationWorsening = 150,
    DegradationImproving = 151,
    DegradationStable = 152,
    DegradationInsufficientSamples = 153,

    // --- Evidence quality and coverage (170..199) -------------------------
    EvidenceRecovered = 170,
    EvidenceSynthetic = 171,
    EvidenceRefused = 172,
    EvidenceTruncated = 173,
    EvidenceSupersededGeneration = 174,
    SourceLoss = 175,
    NoEvidenceForAsset = 176,
    AssessmentLimitedByEvidence = 177,
    FindingLimitReached = 178,
    /// A static statement (lifecycle, maintenance, firmware) whose age exceeds
    /// the static window.
    StaticEvidenceStale = 179,
    /// A static statement dated ahead of the evaluation instant.
    StaticEvidenceFutureDated = 180,

    // --- Risk (200..219) --------------------------------------------------
    RiskModelApplied = 200,
    RiskInputUnavailable = 201,
    RiskModelIncomplete = 202,
};

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(FindingCode code) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<FindingCode> parse_finding_code(std::string_view text) noexcept;

/// Named bits describing what limits an assessment. These are the explicit
/// "stale / conflicting / unsupported / indeterminate / refused" markers: a
/// consumer reads them instead of trying to infer the limit from the state.
enum class AssessmentFlag : std::uint32_t {
    None = 0,
    /// At least one contributing statement is stale.
    StaleEvidence = 1u << 0,
    /// Two sources contradicted each other about the same fact.
    ConflictingEvidence = 1u << 1,
    /// A required metric has no usable reading.
    MissingRequiredEvidence = 1u << 2,
    /// An active maintenance window set aside at least one finding.
    MaskedByMaintenance = 1u << 3,
    /// Firmware state or baseline is unknown.
    UnknownFirmware = 1u << 4,
    /// The asset's lifecycle state is not known.
    UnknownLifecycle = 1u << 5,
    /// The asset's identity has been replaced at least once.
    IdentityReplaced = 1u << 6,
    /// The store was recovered from durable state before this assessment, and at
    /// least one contributing statement predates the recovery.
    RecoveredEvidence = 1u << 7,
    /// At least one contributing statement came from a modelled source.
    SyntheticEvidence = 1u << 8,
    /// The evidence set for this asset confines the conclusion: the state could
    /// not be settled either way.
    Indeterminate = 1u << 9,
    /// A statement was refused, so the evidence this assessment saw is not
    /// everything the producers sent.
    RefusedEvidence = 1u << 10,
    /// Nothing about this asset has been observed at all.
    NoEvidence = 1u << 11,
    /// A degradation indicator is worsening.
    DegradationObserved = 1u << 12,
    /// The evidence set is longer than the assessment's dependency limit, so the
    /// dependency list names the contributing prefix and says it was cut.
    EvidenceListedPartially = 1u << 13,
    /// A source that had been contributing to this asset has stopped, and its
    /// evidence has gone stale.
    SourceLossObserved = 1u << 14,
};

/// A set of assessment flags.
class ASSET_HEALTH_API AssessmentFlags {
public:
    AssessmentFlags() = default;
    explicit AssessmentFlags(std::uint32_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] static AssessmentFlags of(AssessmentFlag flag) noexcept {
        return AssessmentFlags(static_cast<std::uint32_t>(flag));
    }

    void set(AssessmentFlag flag) noexcept { bits_ |= static_cast<std::uint32_t>(flag); }
    [[nodiscard]] bool contains(AssessmentFlag flag) const noexcept {
        return (bits_ & static_cast<std::uint32_t>(flag)) != 0;
    }
    [[nodiscard]] std::uint32_t bits() const noexcept { return bits_; }
    [[nodiscard]] bool none() const noexcept { return bits_ == 0; }

    /// Canonical comma-free space separated list of set flag names, in
    /// declaration order. Empty when no flag is set.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend bool operator==(const AssessmentFlags& lhs, const AssessmentFlags& rhs) noexcept {
        return lhs.bits_ == rhs.bits_;
    }

private:
    std::uint32_t bits_ = 0;
};

/// Every assessment flag in declaration order, for stable iteration.
[[nodiscard]] ASSET_HEALTH_API const std::vector<AssessmentFlag>& all_assessment_flags();
[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(AssessmentFlag flag) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<AssessmentFlag> parse_assessment_flag(std::string_view text) noexcept;

/// One reason inside an assessment.
struct ASSET_HEALTH_API Finding {
    FindingCode code = FindingCode::None;
    FindingSeverity severity = FindingSeverity::Info;
    /// The state this finding supports. A finding with Informational or
    /// Uncertain disposition carries the state it would have supported had the
    /// evidence been usable, so a reader can see what was set aside.
    HealthState impact = HealthState::Healthy;
    FindingDisposition disposition = FindingDisposition::Informational;
    /// Human explanation, deterministic for the same inputs.
    std::string detail;
    /// The metric this finding is about, when it is about one.
    std::optional<MetricKind> metric;
    /// The component this finding is about, when it is about one.
    std::optional<ComponentId> component;
    /// The statements this finding rests on, in canonical order.
    std::vector<EvidenceId> evidence;
    /// The maintenance window that masked this finding, named so the mask is
    /// auditable: "<kind> <start>..<end>".
    std::optional<std::string> masked_by;

    /// Ordering key: disposition, then severity descending, then code, then
    /// metric, then component, then the first evidence identifier. Total.
    [[nodiscard]] static bool canonical_less(const Finding& lhs, const Finding& rhs) noexcept;
};

/// Direction of a trend derived from a bounded window of samples.
enum class TrendKind : std::uint8_t { Worsening = 0, Improving = 1, Stable = 2, Unknown = 3 };

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(TrendKind trend) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<TrendKind> parse_trend_kind(std::string_view text) noexcept;

/// A degradation indicator: an explicit statement that a metric moved in its
/// adverse direction over a stated window, with the samples that show it.
struct DegradationIndicator {
    MetricKind metric = MetricKind::Temperature;
    TrendKind trend = TrendKind::Unknown;
    /// Net change across the window, in thousandths, in the adverse direction
    /// (positive means worse).
    std::int64_t adverse_delta_milli = 0;
    std::size_t sample_count = 0;
    Instant first_observed{};
    Instant last_observed{};
    std::vector<EvidenceId> evidence;
    std::string explanation;
};

/// One statement an assessment depended on, with the freshness that statement
/// had when the assessment was made.
struct EvidenceDependency {
    EvidenceId id;
    EvidenceKind kind = EvidenceKind::TelemetryReading;
    SourceId source;
    SourceKind source_kind = SourceKind::TelemetryFeed;
    SourceClass source_class = SourceClass::Peer;
    Instant observed_at{};
    Freshness freshness = Freshness::Stale;
    bool fresh = false;
    /// True when the statement came from a modelled source.
    bool synthetic = false;
};

/// One input of the replacement-risk model. Every field is printed: a reader can
/// recompute the contribution from the raw value, the saturation point and the
/// weight without reading any code.
struct RiskInput {
    /// Stable canonical name of the input.
    std::string name;
    /// False when the evidence this input needs is absent.
    bool available = false;
    /// The raw input in thousandths of its own unit (hours, counts, a 0..1000
    /// normalized fraction), exactly as measured.
    std::int64_t raw_milli = 0;
    /// The raw value at which this input's score saturates, in the same unit.
    std::int64_t saturation_milli = 1;
    /// The normalized score in [0,1] as an exact rational, before weighting.
    Rational score{};
    /// The weight applied to the score.
    Rational weight{};
    /// score * weight, exactly.
    Rational contribution{};
    /// One line saying what the input measured and where it came from.
    std::string explanation;
    std::vector<EvidenceId> evidence;
};

/// The band a risk total falls in.
enum class RiskBand : std::uint8_t { Undetermined = 0, Low = 1, Moderate = 2, High = 3, Severe = 4 };

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(RiskBand band) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<RiskBand> parse_risk_band(std::string_view text) noexcept;

/// The replacement-risk view. Never a bare number: the band, the exact total,
/// the formula, and every input with its own contribution are all present, and
/// \p complete says whether every input had evidence.
struct RiskView {
    RiskBand band = RiskBand::Undetermined;
    /// Sum of contributions divided by the sum of the weights of the inputs that
    /// were available, exactly. Zero over one when nothing was available.
    Rational total{};
    Rational available_weight_sum{};
    bool complete = false;
    /// The formula, written out, so the number is reproducible from the inputs.
    std::string formula;
    /// One line justifying the band against the configured boundaries.
    std::string justification;
    std::vector<RiskInput> inputs;
};

/// The answer to the core question for one asset at one instant.
struct ASSET_HEALTH_API HealthAssessment {
    AssetRefId asset{};
    AssetGeneration generation{};
    /// Revision of the identity record this assessment was made against. Zero
    /// when no identity statement was observed.
    AssetRevision identity_revision{};
    HealthState state = HealthState::Unknown;
    AssessmentFlags flags{};
    std::vector<Finding> findings;
    std::vector<DegradationIndicator> degradation;
    std::vector<EvidenceDependency> dependencies;
    RiskView risk;
    /// The instant the assessment is about, supplied by the caller.
    Instant evaluated_at{};
    /// Number of admitted statements about this asset and generation.
    std::size_t evidence_considered = 0;
    /// Number of statements the assessment depended on (in the dependency list,
    /// plus any that did not fit the limit).
    std::size_t evidence_depended_on = 0;
    std::string policy_id;
    PolicyVersion policy_version{};
    std::string policy_fingerprint;
    /// Revision of this assessment inside the asset's published history. Zero for
    /// an assessment that was computed but not published.
    AssessmentRevision revision{};

    /// Deterministic multi-line explanation: state, flags, findings, degradation,
    /// dependencies, risk.
    [[nodiscard]] std::string explain() const;

    /// True when the state is not Healthy.
    [[nodiscard]] bool is_attention_required() const noexcept { return state != HealthState::Healthy; }
};

}  // namespace asset_health

#endif  // ASSET_HEALTH_ASSESSMENT_HPP
