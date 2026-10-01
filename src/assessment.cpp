// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/assessment.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "asset_health/limits.hpp"

namespace asset_health {
namespace {

constexpr std::array<std::string_view, kHealthStateCount> kHealthStateNames{{
    "healthy", "unknown", "degraded", "critical", "failed", "retired",
}};

constexpr std::array<std::string_view, kHealthStateCount> kHealthStateDescriptions{{
    "every required reading is present, fresh and inside its thresholds, and no fault stands",
    "the evidence does not settle the question either way",
    "something observed is outside its warning bounds, or an acknowledgement of impairment stands",
    "something observed is outside its critical bounds",
    "an active critical fault stands, or the asset has stopped doing useful work",
    "the asset is decommissioned; service health is not applicable",
}};

constexpr std::array<std::string_view, kFaultSeverityCount> kFindingSeverityNames{{
    "info", "warning", "minor", "major", "critical",
}};

constexpr std::array<std::string_view, kFindingDispositionCount> kFindingDispositionNames{{
    "supported", "masked", "superseded", "refused", "uncertain", "informational",
}};

struct FindingCodeName {
    FindingCode code;
    std::string_view name;
};

/// Every finding code with its stable name. The order of the entries is the
/// declaration order of the enumeration, and a code appears exactly once.
[[nodiscard]] const std::vector<FindingCodeName>& finding_code_names() {
    static const std::vector<FindingCodeName> names{
    {FindingCode::None, "none"},
    {FindingCode::IdentityObserved, "identity-observed"},
    {FindingCode::IdentityNotObserved, "identity-not-observed"},
    {FindingCode::IdentityReplaced, "identity-replaced"},
    {FindingCode::GenerationSuperseded, "generation-superseded"},
    {FindingCode::IdentityRevisionRegressed, "identity-revision-regressed"},
    {FindingCode::MultipleIdentityClaims, "multiple-identity-claims"},
    {FindingCode::LifecycleNotObserved, "lifecycle-not-observed"},
    {FindingCode::LifecycleObserved, "lifecycle-observed"},
    {FindingCode::AssetInService, "asset-in-service"},
    {FindingCode::AssetNotYetInService, "asset-not-yet-in-service"},
    {FindingCode::AssetServiceWithheld, "asset-service-withheld"},
    {FindingCode::AssetEndOfLife, "asset-end-of-life"},
    {FindingCode::AssetDraining, "asset-draining"},
    {FindingCode::LifecycleStale, "lifecycle-stale"},
    {FindingCode::LifecycleConflicting, "lifecycle-conflicting"},
    {FindingCode::AssetServiceImpaired, "asset-service-impaired"},
    {FindingCode::MaintenanceNotObserved, "maintenance-not-observed"},
    {FindingCode::MaintenanceScheduledNotStarted, "maintenance-scheduled-not-started"},
    {FindingCode::MaintenanceWindowActive, "maintenance-window-active"},
    {FindingCode::MaintenanceWindowEnded, "maintenance-window-ended"},
    {FindingCode::MaintenanceCompletedInWindow, "maintenance-completed-in-window"},
    {FindingCode::MaskingApplied, "masking-applied"},
    {FindingCode::MaskingRefused, "masking-refused"},
    {FindingCode::CorrectiveMaintenanceHistory, "corrective-maintenance-history"},
    {FindingCode::FirmwareNotObserved, "firmware-not-observed"},
    {FindingCode::FirmwareMatchesBaseline, "firmware-matches-baseline"},
    {FindingCode::FirmwareBehindBaseline, "firmware-behind-baseline"},
    {FindingCode::FirmwareAheadOfBaseline, "firmware-ahead-of-baseline"},
    {FindingCode::FirmwareBaselineUnknown, "firmware-baseline-unknown"},
    {FindingCode::FirmwareStale, "firmware-stale"},
    {FindingCode::FaultActive, "fault-active"},
    {FindingCode::FaultCleared, "fault-cleared"},
    {FindingCode::FaultDuplicateSuppressed, "fault-duplicate-suppressed"},
    {FindingCode::FaultOutOfOrderSuperseded, "fault-out-of-order-superseded"},
    {FindingCode::FaultDescriptionUnavailable, "fault-description-unavailable"},
    {FindingCode::FaultRecurrence, "fault-recurrence"},
    {FindingCode::FaultStale, "fault-stale"},
    {FindingCode::FaultRecovered, "fault-recovered"},
    {FindingCode::FaultConflicting, "fault-conflicting"},
    {FindingCode::MetricWithinThreshold, "metric-within-threshold"},
    {FindingCode::MetricWarningThreshold, "metric-warning-threshold"},
    {FindingCode::MetricCriticalThreshold, "metric-critical-threshold"},
    {FindingCode::MetricSubstituted, "metric-substituted"},
    {FindingCode::MetricUncertain, "metric-uncertain"},
    {FindingCode::MetricBadQuality, "metric-bad-quality"},
    {FindingCode::MetricMissing, "metric-missing"},
    {FindingCode::MetricStale, "metric-stale"},
    {FindingCode::MetricFutureDated, "metric-future-dated"},
    {FindingCode::MetricRecovered, "metric-recovered"},
    {FindingCode::MetricSourceEpochSuperseded, "metric-source-epoch-superseded"},
    {FindingCode::MetricConflicting, "metric-conflicting"},
    {FindingCode::MetricUnknownMetric, "metric-unknown-metric"},
    {FindingCode::MetricCached, "metric-cached"},
    {FindingCode::DegradationWorsening, "degradation-worsening"},
    {FindingCode::DegradationImproving, "degradation-improving"},
    {FindingCode::DegradationStable, "degradation-stable"},
    {FindingCode::DegradationInsufficientSamples, "degradation-insufficient-samples"},
    {FindingCode::EvidenceRecovered, "evidence-recovered"},
    {FindingCode::EvidenceSynthetic, "evidence-synthetic"},
    {FindingCode::EvidenceRefused, "evidence-refused"},
    {FindingCode::EvidenceTruncated, "evidence-truncated"},
    {FindingCode::EvidenceSupersededGeneration, "evidence-superseded-generation"},
    {FindingCode::SourceLoss, "source-loss"},
    {FindingCode::NoEvidenceForAsset, "no-evidence-for-asset"},
    {FindingCode::AssessmentLimitedByEvidence, "assessment-limited-by-evidence"},
    {FindingCode::FindingLimitReached, "finding-limit-reached"},
    {FindingCode::StaticEvidenceStale, "static-evidence-stale"},
    {FindingCode::StaticEvidenceFutureDated, "static-evidence-future-dated"},
    {FindingCode::RiskModelApplied, "risk-model-applied"},
    {FindingCode::RiskInputUnavailable, "risk-input-unavailable"},
    {FindingCode::RiskModelIncomplete, "risk-model-incomplete"},
    };
    return names;
}

struct AssessmentFlagName {
    AssessmentFlag flag;
    std::string_view name;
};

constexpr std::array<AssessmentFlagName, 15> kAssessmentFlagNames{{
    {AssessmentFlag::StaleEvidence, "stale-evidence"},
    {AssessmentFlag::ConflictingEvidence, "conflicting-evidence"},
    {AssessmentFlag::MissingRequiredEvidence, "missing-required-evidence"},
    {AssessmentFlag::MaskedByMaintenance, "masked-by-maintenance"},
    {AssessmentFlag::UnknownFirmware, "unknown-firmware"},
    {AssessmentFlag::UnknownLifecycle, "unknown-lifecycle"},
    {AssessmentFlag::IdentityReplaced, "identity-replaced"},
    {AssessmentFlag::RecoveredEvidence, "recovered-evidence"},
    {AssessmentFlag::SyntheticEvidence, "synthetic-evidence"},
    {AssessmentFlag::Indeterminate, "indeterminate"},
    {AssessmentFlag::RefusedEvidence, "refused-evidence"},
    {AssessmentFlag::NoEvidence, "no-evidence"},
    {AssessmentFlag::DegradationObserved, "degradation-observed"},
    {AssessmentFlag::EvidenceListedPartially, "evidence-listed-partially"},
    {AssessmentFlag::SourceLossObserved, "source-loss-observed"},
}};

constexpr std::array<std::string_view, 4> kTrendKindNames{{"worsening", "improving", "stable", "unknown"}};
constexpr std::array<std::string_view, 5> kRiskBandNames{{"undetermined", "low", "moderate", "high", "severe"}};

[[nodiscard]] std::string join_evidence(const std::vector<EvidenceId>& ids) {
    std::string text;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) {
            text.push_back(',');
        }
        text.append(ids[index].to_string());
    }
    return text;
}

}  // namespace

std::string_view to_string(HealthState state) noexcept {
    const auto index = static_cast<std::size_t>(state);
    return index < kHealthStateNames.size() ? kHealthStateNames[index] : std::string_view{"unknown"};
}

const char* health_state_description(HealthState state) noexcept {
    const auto index = static_cast<std::size_t>(state);
    return index < kHealthStateDescriptions.size() ? kHealthStateDescriptions[index].data() : "not a health state";
}

std::optional<HealthState> parse_health_state(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kHealthStateNames.size(); ++index) {
        if (kHealthStateNames[index] == text) {
            return static_cast<HealthState>(index);
        }
    }
    return std::nullopt;
}

std::uint8_t health_state_rank(HealthState state) noexcept { return static_cast<std::uint8_t>(state); }

HealthState worse_of(HealthState lhs, HealthState rhs) noexcept {
    return health_state_rank(lhs) >= health_state_rank(rhs) ? lhs : rhs;
}

std::string_view to_string(FindingSeverity severity) noexcept {
    const auto index = static_cast<std::size_t>(severity);
    return index < kFindingSeverityNames.size() ? kFindingSeverityNames[index] : std::string_view{"unknown"};
}

std::optional<FindingSeverity> parse_finding_severity(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kFindingSeverityNames.size(); ++index) {
        if (kFindingSeverityNames[index] == text) {
            return static_cast<FindingSeverity>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(FindingDisposition disposition) noexcept {
    const auto index = static_cast<std::size_t>(disposition);
    return index < kFindingDispositionNames.size() ? kFindingDispositionNames[index] : std::string_view{"unknown"};
}

std::optional<FindingDisposition> parse_finding_disposition(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kFindingDispositionNames.size(); ++index) {
        if (kFindingDispositionNames[index] == text) {
            return static_cast<FindingDisposition>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(FindingCode code) noexcept {
    for (const auto& entry : finding_code_names()) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<FindingCode> parse_finding_code(std::string_view text) noexcept {
    for (const auto& entry : finding_code_names()) {
        if (entry.name == text) {
            return entry.code;
        }
    }
    return std::nullopt;
}

const std::vector<AssessmentFlag>& all_assessment_flags() {
    static const std::vector<AssessmentFlag> flags{
        AssessmentFlag::StaleEvidence,
        AssessmentFlag::ConflictingEvidence,
        AssessmentFlag::MissingRequiredEvidence,
        AssessmentFlag::MaskedByMaintenance,
        AssessmentFlag::UnknownFirmware,
        AssessmentFlag::UnknownLifecycle,
        AssessmentFlag::IdentityReplaced,
        AssessmentFlag::RecoveredEvidence,
        AssessmentFlag::SyntheticEvidence,
        AssessmentFlag::Indeterminate,
        AssessmentFlag::RefusedEvidence,
        AssessmentFlag::NoEvidence,
        AssessmentFlag::DegradationObserved,
        AssessmentFlag::EvidenceListedPartially,
        AssessmentFlag::SourceLossObserved,
    };
    return flags;
}

std::string_view to_string(AssessmentFlag flag) noexcept {
    for (const auto& entry : kAssessmentFlagNames) {
        if (entry.flag == flag) {
            return entry.name;
        }
    }
    return "unknown";
}

std::optional<AssessmentFlag> parse_assessment_flag(std::string_view text) noexcept {
    for (const auto& entry : kAssessmentFlagNames) {
        if (entry.name == text) {
            return entry.flag;
        }
    }
    return std::nullopt;
}

std::string AssessmentFlags::to_string() const {
    std::string text;
    for (const AssessmentFlag flag : all_assessment_flags()) {
        if (!contains(flag)) {
            continue;
        }
        if (!text.empty()) {
            text.push_back(' ');
        }
        text.append(asset_health::to_string(flag));
    }
    return text;
}

bool Finding::canonical_less(const Finding& lhs, const Finding& rhs) noexcept {
    if (lhs.disposition != rhs.disposition) {
        return lhs.disposition < rhs.disposition;
    }
    if (lhs.severity != rhs.severity) {
        return rhs.severity < lhs.severity;
    }
    if (lhs.code != rhs.code) {
        return lhs.code < rhs.code;
    }
    const int lhs_metric = lhs.metric.has_value() ? static_cast<int>(*lhs.metric) : -1;
    const int rhs_metric = rhs.metric.has_value() ? static_cast<int>(*rhs.metric) : -1;
    if (lhs_metric != rhs_metric) {
        return lhs_metric < rhs_metric;
    }
    const std::string lhs_component = lhs.component.has_value() ? lhs.component->str() : std::string();
    const std::string rhs_component = rhs.component.has_value() ? rhs.component->str() : std::string();
    if (lhs_component != rhs_component) {
        return lhs_component < rhs_component;
    }
    const EvidenceId lhs_first = lhs.evidence.empty() ? EvidenceId::nil() : lhs.evidence.front();
    const EvidenceId rhs_first = rhs.evidence.empty() ? EvidenceId::nil() : rhs.evidence.front();
    return lhs_first < rhs_first;
}

std::string_view to_string(TrendKind trend) noexcept {
    const auto index = static_cast<std::size_t>(trend);
    return index < kTrendKindNames.size() ? kTrendKindNames[index] : std::string_view{"unknown"};
}

std::optional<TrendKind> parse_trend_kind(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kTrendKindNames.size(); ++index) {
        if (kTrendKindNames[index] == text) {
            return static_cast<TrendKind>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(RiskBand band) noexcept {
    const auto index = static_cast<std::size_t>(band);
    return index < kRiskBandNames.size() ? kRiskBandNames[index] : std::string_view{"unknown"};
}

std::optional<RiskBand> parse_risk_band(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kRiskBandNames.size(); ++index) {
        if (kRiskBandNames[index] == text) {
            return static_cast<RiskBand>(index);
        }
    }
    return std::nullopt;
}

std::string HealthAssessment::explain() const {
    std::string text;
    text.append("asset ");
    text.append(asset.to_string());
    text.append(" generation ");
    text.append(generation.to_string());
    text.append(" identity-revision ");
    text.append(identity_revision.to_string());
    text.push_back('\n');
    text.append("state ");
    text.append(to_string(state));
    text.append(" flags ");
    const std::string flags_text = this->flags.to_string();
    text.append(flags_text.empty() ? std::string("none") : flags_text);
    text.push_back('\n');
    text.append("evaluated ");
    text.append(evaluated_at.to_string());
    text.append(" policy ");
    text.append(policy_id);
    text.append(" version ");
    text.append(policy_version.to_string());
    text.append(" fingerprint ");
    text.append(policy_fingerprint);
    text.push_back('\n');
    text.append("evidence considered ");
    text.append(std::to_string(evidence_considered));
    text.append(" depended-on ");
    text.append(std::to_string(evidence_depended_on));
    text.append(" dependencies-listed ");
    text.append(std::to_string(dependencies.size()));
    text.push_back('\n');
    text.append("assessment-revision ");
    text.append(revision.to_string());
    text.push_back('\n');
    for (const Finding& finding : findings) {
        text.append("finding ");
        text.append(to_string(finding.severity));
        text.push_back(' ');
        text.append(to_string(finding.code));
        text.push_back(' ');
        text.append(to_string(finding.disposition));
        text.append(" impact=");
        text.append(to_string(finding.impact));
        if (finding.metric.has_value()) {
            text.append(" metric=");
            text.append(to_string(*finding.metric));
        }
        if (finding.component.has_value()) {
            text.append(" component=");
            text.append(finding.component->str());
        }
        if (!finding.evidence.empty()) {
            text.append(" evidence=");
            text.append(join_evidence(finding.evidence));
        }
        if (finding.masked_by.has_value()) {
            text.append(" masked-by=");
            text.append(*finding.masked_by);
        }
        text.append(" -- ");
        text.append(finding.detail);
        text.push_back('\n');
    }
    for (const DegradationIndicator& indicator : degradation) {
        text.append("degradation ");
        text.append(to_string(indicator.metric));
        text.push_back(' ');
        text.append(to_string(indicator.trend));
        text.append(" adverse-delta-milli=");
        text.append(std::to_string(indicator.adverse_delta_milli));
        text.append(" samples=");
        text.append(std::to_string(indicator.sample_count));
        text.append(" window ");
        text.append(indicator.first_observed.to_millisecond_string());
        text.append("..");
        text.append(indicator.last_observed.to_millisecond_string());
        text.append(" -- ");
        text.append(indicator.explanation);
        text.push_back('\n');
    }
    for (const EvidenceDependency& dependency : dependencies) {
        text.append("dependency ");
        text.append(dependency.id.to_string());
        text.push_back(' ');
        text.append(to_string(dependency.kind));
        text.append(" source ");
        text.append(dependency.source.str());
        text.append(" class ");
        text.append(to_string(dependency.source_class));
        text.append(" observed ");
        text.append(dependency.observed_at.to_millisecond_string());
        text.append(" freshness ");
        text.append(to_string(dependency.freshness));
        text.append(" fresh=");
        text.append(dependency.fresh ? "true" : "false");
        if (dependency.synthetic) {
            text.append(" synthetic=true");
        }
        text.push_back('\n');
    }
    text.append("risk band ");
    text.append(to_string(risk.band));
    text.append(" total ");
    text.append(risk.total.to_string());
    text.append(" (");
    text.append(risk.total.to_decimal_string(4));
    text.append(") complete ");
    text.append(risk.complete ? "true" : "false");
    text.append(" available-weight ");
    text.append(risk.available_weight_sum.to_string());
    text.push_back('\n');
    text.append("risk formula ");
    text.append(risk.formula);
    text.push_back('\n');
    text.append("risk justification ");
    text.append(risk.justification);
    text.push_back('\n');
    for (const RiskInput& input : risk.inputs) {
        text.append("risk-input ");
        text.append(input.name);
        text.append(" available=");
        text.append(input.available ? "true" : "false");
        text.append(" raw-milli=");
        text.append(std::to_string(input.raw_milli));
        text.append(" saturation-milli=");
        text.append(std::to_string(input.saturation_milli));
        text.append(" score=");
        text.append(input.score.to_string());
        text.append(" weight=");
        text.append(input.weight.to_string());
        text.append(" contribution=");
        text.append(input.contribution.to_string());
        if (!input.evidence.empty()) {
            text.append(" evidence=");
            text.append(join_evidence(input.evidence));
        }
        text.append(" -- ");
        text.append(input.explanation);
        text.push_back('\n');
    }
    return text;
}

}  // namespace asset_health
