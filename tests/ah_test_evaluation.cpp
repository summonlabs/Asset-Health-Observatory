// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The evaluator, exercised as a pure function: the same evidence, policy and
// instant must always produce the same answer, and every reason must be visible.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "asset_health/evaluation.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

const Instant kBase = instant("2026-01-01T00:00:00Z");

/// Collects statements and evaluates them for one asset and incarnation.
class Fixture {
public:
    explicit Fixture(AssetRefId asset = asset_id(kAssetA)) : asset_(asset) {}

    void add(const Outcome<EvidenceRecord>& record) {
        AH_REQUIRE(record.has_value());
        records_.push_back(record.value().with_commit_epoch(StoreEpoch(1)));
    }

    void add(const EvidenceRecord& record) { records_.push_back(record.with_commit_epoch(StoreEpoch(1))); }

    [[nodiscard]] HealthAssessment run(Instant now, StoreEpoch epoch = StoreEpoch(1),
                                       bool recovered = false) const {
        EvaluationRequest request;
        request.asset = asset_;
        request.generation = AssetGeneration(1);
        request.evidence = records_;
        EvaluationContext context;
        context.now = now;
        context.store_epoch = epoch;
        context.store_recovered = recovered;
        return evaluate(request, HealthPolicy::standard(), context);
    }

    [[nodiscard]] std::vector<EvidenceRecord>& records() { return records_; }

private:
    AssetRefId asset_;
    std::vector<EvidenceRecord> records_;
};

[[nodiscard]] bool has_finding(const HealthAssessment& assessment, FindingCode code) {
    return std::any_of(assessment.findings.begin(), assessment.findings.end(),
                       [code](const Finding& finding) { return finding.code == code; });
}

[[nodiscard]] const Finding* find_finding(const HealthAssessment& assessment, FindingCode code) {
    for (const Finding& finding : assessment.findings) {
        if (finding.code == code) {
            return &finding;
        }
    }
    return nullptr;
}

Builder feed(const char* source = "feed-a", SourceClass klass = SourceClass::Peer) {
    return Builder(asset_id(kAssetA), *SourceId::parse(source), SourceKind::TelemetryFeed, klass);
}

Builder registry() {
    return Builder(asset_id(kAssetA), *SourceId::parse("registry-a"), SourceKind::AssetRegistry,
                   SourceClass::Authoritative);
}

Builder lifecycle_source() {
    return Builder(asset_id(kAssetA), *SourceId::parse("lifecycle-a"), SourceKind::HardwareLifecycle,
                   SourceClass::Authoritative);
}

Builder maintenance_source() {
    return Builder(asset_id(kAssetA), *SourceId::parse("maint-a"), SourceKind::MaintenanceCoordinator,
                   SourceClass::Authoritative);
}

Builder fault_source() {
    return Builder(asset_id(kAssetA), *SourceId::parse("faults-a"), SourceKind::FaultFeed, SourceClass::Peer);
}

Builder firmware_source() {
    return Builder(asset_id(kAssetA), *SourceId::parse("firmware-a"), SourceKind::FirmwareBaseline,
                   SourceClass::Authoritative);
}

/// A fixture with identity, an active lifecycle state and one good reading.
Fixture healthy_fixture(std::int64_t temperature_milli = 40000) {
    Fixture fixture;
    fixture.add(EvidenceRecord::make(registry().observed(kBase).identity(3)));
    fixture.add(EvidenceRecord::make(
        lifecycle_source().observed(kBase).lifecycle(LifecycleState::Active, kBase)));
    fixture.add(EvidenceRecord::make(
        feed().observed(kBase).telemetry(MetricKind::Temperature, temperature_milli, SampleQuality::Good)));
    return fixture;
}

}  // namespace

AH_TEST(evaluation, healthy_when_every_required_reading_is_present_and_fresh) {
    const Fixture fixture = healthy_fixture();
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(60)));
    AH_CHECK(assessment.state == HealthState::Healthy);
    AH_CHECK(assessment.flags.contains(AssessmentFlag::UnknownFirmware));
    AH_CHECK(has_finding(assessment, FindingCode::MetricWithinThreshold));
    AH_CHECK(has_finding(assessment, FindingCode::IdentityObserved));
    AH_CHECK(has_finding(assessment, FindingCode::AssetInService));
    AH_CHECK(assessment.evidence_considered == 3);
    AH_CHECK(assessment.evidence_depended_on == 3);
    AH_CHECK(assessment.dependencies.size() == 3);
    AH_CHECK(!assessment.risk.inputs.empty());
    AH_CHECK(assessment.policy_fingerprint == HealthPolicy::standard().fingerprint());
}

AH_TEST(evaluation, missing_required_telemetry_is_unknown_and_never_healthy) {
    Fixture fixture;
    fixture.add(EvidenceRecord::make(registry().observed(kBase).identity(3)));
    fixture.add(EvidenceRecord::make(lifecycle_source().observed(kBase).lifecycle(LifecycleState::Active, kBase)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(assessment.state == HealthState::Unknown);
    AH_CHECK(assessment.flags.contains(AssessmentFlag::MissingRequiredEvidence));
    const Finding* missing = find_finding(assessment, FindingCode::MetricMissing);
    AH_REQUIRE(missing != nullptr);
    AH_CHECK(missing->disposition == FindingDisposition::Supported);
    AH_CHECK(missing->metric.has_value());
    AH_CHECK(*missing->metric == MetricKind::Temperature);
}

AH_TEST(evaluation, a_stale_reading_stops_counting_and_is_named) {
    Fixture fixture = healthy_fixture();
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(3600)));
    AH_CHECK(assessment.state == HealthState::Unknown);
    AH_CHECK(assessment.flags.contains(AssessmentFlag::StaleEvidence));
    AH_CHECK(assessment.flags.contains(AssessmentFlag::SourceLossObserved));
    AH_CHECK(has_finding(assessment, FindingCode::MetricStale));
    AH_CHECK(has_finding(assessment, FindingCode::SourceLoss));
}

AH_TEST(evaluation, a_reading_recovered_from_a_previous_epoch_is_reported_as_recovered) {
    Fixture fixture = healthy_fixture();
    // An orderly restart: the reading was committed under epoch 1 and the store
    // is at epoch 2, but the session that wrote it closed cleanly, so the reading
    // is still judged by its age and the answer is healthy.
    const HealthAssessment orderly =
        fixture.run(*kBase.shifted(Duration::from_seconds(1)), StoreEpoch(2), false);
    AH_CHECK(orderly.state == HealthState::Healthy);
    AH_CHECK(!has_finding(orderly, FindingCode::MetricRecovered));

    // A session that died: nothing has confirmed the reading since it stopped, so
    // it is recovered evidence and does not count.
    const HealthAssessment recovered_run =
        fixture.run(*kBase.shifted(Duration::from_seconds(1)), StoreEpoch(2), true);
    AH_CHECK(recovered_run.state == HealthState::Unknown);
    AH_CHECK(has_finding(recovered_run, FindingCode::MetricRecovered));
    const Finding* recovered = find_finding(recovered_run, FindingCode::MetricRecovered);
    AH_REQUIRE(recovered != nullptr);
    AH_CHECK(recovered->detail.find("recovered") != std::string::npos);
}

AH_TEST(evaluation, thresholds_support_warning_and_critical_states) {
    const HealthAssessment warning = healthy_fixture(80000).run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(warning.state == HealthState::Degraded);
    AH_CHECK(has_finding(warning, FindingCode::MetricWarningThreshold));

    const HealthAssessment critical = healthy_fixture(90000).run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(critical.state == HealthState::Critical);
    AH_CHECK(has_finding(critical, FindingCode::MetricCriticalThreshold));
}

AH_TEST(evaluation, conflicting_sensors_are_both_preserved_and_never_healthy) {
    Fixture fixture = healthy_fixture();
    fixture.add(EvidenceRecord::make(
        feed("feed-b").observed(kBase).telemetry(MetricKind::Temperature, 55000, SampleQuality::Good)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(assessment.state == HealthState::Unknown);
    AH_CHECK(assessment.flags.contains(AssessmentFlag::ConflictingEvidence));
    const Finding* conflict = find_finding(assessment, FindingCode::MetricConflicting);
    AH_REQUIRE(conflict != nullptr);
    AH_CHECK(conflict->evidence.size() == 2);
    AH_CHECK(conflict->detail.find("both readings are preserved") != std::string::npos);
}

AH_TEST(evaluation, readings_inside_the_conflict_tolerance_do_not_conflict) {
    Fixture fixture = healthy_fixture(40000);
    fixture.add(EvidenceRecord::make(
        feed("feed-b").observed(kBase).telemetry(MetricKind::Temperature, 41000, SampleQuality::Good)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(!assessment.flags.contains(AssessmentFlag::ConflictingEvidence));
    AH_CHECK(assessment.state == HealthState::Healthy);
}

AH_TEST(evaluation, an_active_critical_fault_supports_failed) {
    Fixture fixture = healthy_fixture();
    fixture.add(EvidenceRecord::make(
        fault_source().observed(*kBase.shifted(Duration::from_seconds(5))).fault("pump-1", "coolant-loss",
                                                                                FaultSeverity::Critical,
                                                                                FaultStatus::Active)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(assessment.state == HealthState::Failed);
    const Finding* fault = find_finding(assessment, FindingCode::FaultActive);
    AH_REQUIRE(fault != nullptr);
    AH_CHECK(fault->disposition == FindingDisposition::Supported);
    AH_CHECK(fault->impact == HealthState::Failed);
}

AH_TEST(evaluation, an_old_active_fault_is_reported_but_does_not_assert_itself) {
    Fixture fixture = healthy_fixture();
    fixture.add(EvidenceRecord::make(fault_source().observed(kBase).fault("pump-1", "coolant-loss",
                                                                          FaultSeverity::Critical,
                                                                          FaultStatus::Active)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_hours(2)));
    AH_CHECK(assessment.state == HealthState::Unknown);
    const Finding* fault = find_finding(assessment, FindingCode::FaultStale);
    AH_REQUIRE(fault != nullptr);
    AH_CHECK(fault->disposition == FindingDisposition::Uncertain);
    AH_CHECK(fault->impact == HealthState::Failed);
}

AH_TEST(evaluation, duplicate_faults_collapse_and_out_of_order_statements_are_superseded) {
    Fixture fixture = healthy_fixture();
    const Instant first = *kBase.shifted(Duration::from_seconds(10));
    const Instant second = *kBase.shifted(Duration::from_seconds(20));
    fixture.add(EvidenceRecord::make(
        fault_source().observed(first).sequence(1).fault("psu-1", "psu-ovt", FaultSeverity::Major,
                                                         FaultStatus::Active)));
    fixture.add(EvidenceRecord::make(
        fault_source().observed(second).sequence(2).fault("psu-1", "psu-ovt", FaultSeverity::Major,
                                                          FaultStatus::Cleared)));
    // A late arrival of an older statement: it must not resurrect the fault.
    fixture.add(EvidenceRecord::make(
        fault_source().observed(first).sequence(3).epoch(2).fault("psu-1", "psu-ovt", FaultSeverity::Major,
                                                                  FaultStatus::Active)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(30)));
    AH_CHECK(has_finding(assessment, FindingCode::FaultCleared));
    AH_CHECK(!has_finding(assessment, FindingCode::FaultActive));
    AH_CHECK(has_finding(assessment, FindingCode::FaultDuplicateSuppressed) ||
             has_finding(assessment, FindingCode::FaultOutOfOrderSuperseded));
}

AH_TEST(evaluation, maintenance_masking_sets_a_finding_aside_without_hiding_it) {
    Fixture fixture = healthy_fixture(90000);
    fixture.add(EvidenceRecord::make(maintenance_source().observed(kBase).maintenance(
        MaintenanceKind::Planned, MaintenanceState::Active, kBase, *kBase.shifted(Duration::from_hours(4)),
        MaskScope::Telemetry)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(60)));
    AH_CHECK(assessment.flags.contains(AssessmentFlag::MaskedByMaintenance));
    const Finding* masked = find_finding(assessment, FindingCode::MetricCriticalThreshold);
    AH_REQUIRE(masked != nullptr);
    AH_CHECK(masked->disposition == FindingDisposition::Masked);
    AH_CHECK(masked->masked_by.has_value());
    AH_CHECK(masked->impact == HealthState::Critical);
    AH_CHECK(assessment.state != HealthState::Critical);
}

AH_TEST(evaluation, masking_is_refused_for_corrective_work) {
    Fixture fixture = healthy_fixture(90000);
    fixture.add(EvidenceRecord::make(maintenance_source().observed(kBase).maintenance(
        MaintenanceKind::Corrective, MaintenanceState::Active, kBase, *kBase.shifted(Duration::from_hours(4)),
        MaskScope::Telemetry)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(60)));
    AH_CHECK(!assessment.flags.contains(AssessmentFlag::MaskedByMaintenance));
    AH_CHECK(has_finding(assessment, FindingCode::MaskingRefused));
    AH_CHECK(assessment.state == HealthState::Critical);
}

AH_TEST(evaluation, a_window_that_has_run_out_stops_masking) {
    Fixture fixture = healthy_fixture(90000);
    fixture.add(EvidenceRecord::make(maintenance_source().observed(kBase).maintenance(
        MaintenanceKind::Planned, MaintenanceState::Active, kBase, *kBase.shifted(Duration::from_seconds(30)),
        MaskScope::Telemetry)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(60)));
    AH_CHECK(!assessment.flags.contains(AssessmentFlag::MaskedByMaintenance));
    AH_CHECK(assessment.state == HealthState::Critical);
    AH_CHECK(has_finding(assessment, FindingCode::MaintenanceWindowEnded));
}

AH_TEST(evaluation, lifecycle_unknown_caps_the_answer_and_end_of_life_is_terminal) {
    Fixture fixture;
    fixture.add(EvidenceRecord::make(registry().observed(kBase).identity(1)));
    fixture.add(EvidenceRecord::make(feed().observed(kBase).telemetry(MetricKind::Temperature, 40000,
                                                                     SampleQuality::Good)));
    const HealthAssessment unknown_lifecycle = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(unknown_lifecycle.state == HealthState::Unknown);
    AH_CHECK(unknown_lifecycle.flags.contains(AssessmentFlag::UnknownLifecycle));
    AH_CHECK(has_finding(unknown_lifecycle, FindingCode::LifecycleNotObserved));

    Fixture retired;
    retired.add(EvidenceRecord::make(registry().observed(kBase).identity(1)));
    retired.add(EvidenceRecord::make(lifecycle_source().observed(kBase).lifecycle(LifecycleState::Retired,
                                                                                  std::nullopt)));
    retired.add(EvidenceRecord::make(fault_source().observed(kBase).fault("psu-1", "psu-ovt", FaultSeverity::Critical,
                                                                        FaultStatus::Active)));
    const HealthAssessment end_of_life = retired.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(end_of_life.state == HealthState::Retired);
    AH_CHECK(has_finding(end_of_life, FindingCode::AssetEndOfLife));
    AH_CHECK(has_finding(end_of_life, FindingCode::FaultActive));
}

AH_TEST(evaluation, an_acknowledged_impairment_supports_degraded) {
    Fixture fixture;
    fixture.add(EvidenceRecord::make(registry().observed(kBase).identity(1)));
    fixture.add(EvidenceRecord::make(
        lifecycle_source().observed(kBase).lifecycle(LifecycleState::Degraded, std::nullopt)));
    fixture.add(EvidenceRecord::make(
        feed().observed(kBase).telemetry(MetricKind::Temperature, 40000, SampleQuality::Good)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(assessment.state == HealthState::Degraded);
    AH_CHECK(has_finding(assessment, FindingCode::AssetServiceImpaired));
}

AH_TEST(evaluation, evidence_about_another_incarnation_is_excluded_and_reported) {
    Fixture fixture = healthy_fixture();
    auto other = feed().generation(AssetGeneration(2)).observed(kBase).telemetry(MetricKind::Temperature, 99000,
                                                                                SampleQuality::Good);
    fixture.add(EvidenceRecord::make(other));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(has_finding(assessment, FindingCode::GenerationSuperseded));
    // A later incarnation is on record, so this one may already be gone: the
    // answer for it is unknown, and the flag says why.
    AH_CHECK(assessment.flags.contains(AssessmentFlag::IdentityReplaced));
    AH_CHECK(assessment.state == HealthState::Unknown);
    const Finding* superseded = find_finding(assessment, FindingCode::GenerationSuperseded);
    AH_REQUIRE(superseded != nullptr);
    AH_CHECK(superseded->disposition == FindingDisposition::Superseded);
    AH_CHECK(superseded->evidence.size() == 1);
}

AH_TEST(evaluation, firmware_unknown_and_below_baseline_are_distinguished) {
    Fixture without; 
    without.add(EvidenceRecord::make(registry().observed(kBase).identity(1)));
    without.add(EvidenceRecord::make(lifecycle_source().observed(kBase).lifecycle(LifecycleState::Active, kBase)));
    without.add(EvidenceRecord::make(feed().observed(kBase).telemetry(MetricKind::Temperature, 40000,
                                                                     SampleQuality::Good)));
    const HealthAssessment no_firmware = without.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(no_firmware.flags.contains(AssessmentFlag::UnknownFirmware));

    Fixture matching = without;
    matching.add(EvidenceRecord::make(firmware_source().observed(kBase).firmware("gpu-0", "2.1.0", "2.1.0",
                                                                               FirmwareCompliance::Matches)));
    const HealthAssessment matched = matching.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(!matched.flags.contains(AssessmentFlag::UnknownFirmware));
    AH_CHECK(has_finding(matched, FindingCode::FirmwareMatchesBaseline));
}

AH_TEST(evaluation, a_worsening_trend_is_detected_with_its_samples) {
    Fixture fixture = healthy_fixture(40000);
    for (int index = 1; index <= 5; ++index) {
        const auto observed = *kBase.shifted(Duration::from_seconds(60 * index));
        fixture.add(EvidenceRecord::make(feed().observed(observed).sequence(static_cast<std::uint64_t>(10 + index))
                                             .telemetry(MetricKind::Temperature, 40000 + (index * 2000),
                                                        SampleQuality::Good)));
    }
    // Evaluated at the instant of the last reading: everything earlier is inside
    // the five minute dynamic window, so all six readings contribute.
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(300)));
    AH_CHECK(assessment.flags.contains(AssessmentFlag::DegradationObserved));
    AH_CHECK(!assessment.degradation.empty());
    AH_CHECK(assessment.degradation[0].trend == TrendKind::Worsening);
    const std::string observed = "metric=" + std::string(to_string(assessment.degradation[0].metric)) +
                                " trend=" + std::string(to_string(assessment.degradation[0].trend)) +
                                " delta=" + std::to_string(assessment.degradation[0].adverse_delta_milli) +
                                " samples=" + std::to_string(assessment.degradation[0].sample_count) +
                                " indicators=" + std::to_string(assessment.degradation.size());
    AH_CHECK_MSG(assessment.degradation[0].adverse_delta_milli == 10000, observed);
    AH_CHECK_MSG(assessment.degradation[0].sample_count == 6, observed);
    AH_CHECK(assessment.state == HealthState::Degraded);
}

AH_TEST(evaluation, a_counter_reset_is_not_a_trend) {
    Fixture fixture = healthy_fixture(40000);
    fixture.add(EvidenceRecord::make(feed().observed(*kBase.shifted(Duration::from_seconds(10))).sequence(11)
                                         .telemetry(MetricKind::EccCorrectable, 900, SampleQuality::Good)));
    fixture.add(EvidenceRecord::make(feed().observed(*kBase.shifted(Duration::from_seconds(20))).sequence(12)
                                         .telemetry(MetricKind::EccCorrectable, 950, SampleQuality::Good)));
    fixture.add(EvidenceRecord::make(feed().observed(*kBase.shifted(Duration::from_seconds(30))).sequence(13)
                                         .telemetry(MetricKind::EccCorrectable, 10, SampleQuality::Good)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(40)));
    AH_CHECK(!assessment.flags.contains(AssessmentFlag::DegradationObserved));
    bool found_reset = false;
    for (const DegradationIndicator& indicator : assessment.degradation) {
        if (indicator.metric == MetricKind::EccCorrectable) {
            AH_CHECK(indicator.trend == TrendKind::Unknown);
            found_reset = true;
        }
    }
    AH_CHECK(found_reset);
}

AH_TEST(evaluation, an_uncertain_reading_is_not_promoted_to_a_healthy_verdict) {
    Fixture fixture;
    fixture.add(EvidenceRecord::make(registry().observed(kBase).identity(1)));
    fixture.add(EvidenceRecord::make(lifecycle_source().observed(kBase).lifecycle(LifecycleState::Active, kBase)));
    fixture.add(EvidenceRecord::make(feed().observed(kBase).telemetry(MetricKind::Temperature, 40000,
                                                                     SampleQuality::Uncertain)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(assessment.state == HealthState::Unknown);
    AH_CHECK(has_finding(assessment, FindingCode::MetricUncertain));
}

AH_TEST(evaluation, risk_is_recomputable_from_the_reported_inputs) {
    Fixture fixture = healthy_fixture(90000);
    fixture.add(EvidenceRecord::make(fault_source().observed(kBase).fault("psu-1", "psu-ovt", FaultSeverity::Major,
                                                                         FaultStatus::Active)));
    const HealthAssessment assessment = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_REQUIRE(!assessment.risk.inputs.empty());
    Rational weight_sum = Rational::from_integer(0);
    Rational contribution_sum = Rational::from_integer(0);
    for (const RiskInput& input : assessment.risk.inputs) {
        if (!input.available) {
            continue;
        }
        weight_sum = weight_sum.add(input.weight).value();
        contribution_sum = contribution_sum.add(input.contribution).value();
        const std::int64_t bounded = std::min(input.raw_milli, input.saturation_milli);
        AH_CHECK(input.score == Rational::make(bounded, input.saturation_milli).value());
    }
    AH_CHECK(assessment.risk.available_weight_sum == weight_sum);
    const Rational recomputed = contribution_sum.divide(weight_sum).value();
    AH_CHECK(assessment.risk.total == recomputed);
    AH_CHECK(!assessment.risk.formula.empty());
    AH_CHECK(!assessment.risk.justification.empty());
    AH_CHECK(has_finding(assessment, FindingCode::RiskModelApplied));
    AH_CHECK(has_finding(assessment, FindingCode::RiskInputUnavailable));
    AH_CHECK(!assessment.risk.complete);
}

AH_TEST(evaluation, the_same_inputs_produce_the_same_explanation) {
    Fixture fixture = healthy_fixture(80000);
    fixture.add(EvidenceRecord::make(
        fault_source().observed(kBase).fault("psu-1", "psu-ovt", FaultSeverity::Minor, FaultStatus::Active)));
    const HealthAssessment first = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    const HealthAssessment second = fixture.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(first.explain() == second.explain());
    AH_CHECK(first.state == second.state);
    AH_CHECK(first.findings.size() == second.findings.size());
    AH_CHECK(first.risk.total == second.risk.total);

    // Reversing the input order must not change the answer either.
    Fixture reversed;
    for (auto iterator = fixture.records().rbegin(); iterator != fixture.records().rend(); ++iterator) {
        reversed.add(*iterator);
    }
    const HealthAssessment third = reversed.run(*kBase.shifted(Duration::from_seconds(10)));
    AH_CHECK(third.explain() == first.explain());
}

AH_TEST(evaluation, a_refused_statement_is_reported_without_becoming_evidence) {
    Fixture fixture = healthy_fixture();
    EvaluationRequest request;
    request.asset = asset_id(kAssetA);
    request.generation = AssetGeneration(1);
    request.evidence = fixture.records();
    RefusalRecord refusal;
    refusal.asset = asset_id(kAssetA);
    refusal.generation = AssetGeneration(1);
    refusal.source = *SourceId::parse("feed-z");
    refusal.source_kind = SourceKind::TelemetryFeed;
    refusal.claimed_kind = EvidenceKind::LifecycleStatement;
    refusal.code = ErrorCode::AuthorityDomainViolation;
    refusal.detail = "the source does not speak for the domain that owns this evidence";
    refusal.received_at = kBase;
    request.refusals.push_back(refusal);
    EvaluationContext context;
    context.now = *kBase.shifted(Duration::from_seconds(10));
    context.store_epoch = StoreEpoch(1);
    const HealthAssessment assessment = evaluate(request, HealthPolicy::standard(), context);
    AH_CHECK(assessment.flags.contains(AssessmentFlag::RefusedEvidence));
    AH_CHECK(has_finding(assessment, FindingCode::EvidenceRefused));
    const Finding* refused = find_finding(assessment, FindingCode::EvidenceRefused);
    AH_REQUIRE(refused != nullptr);
    AH_CHECK(refused->disposition == FindingDisposition::Refused);
    AH_CHECK(assessment.state == HealthState::Unknown);
}

AH_TEST(evaluation, an_asset_with_no_statements_says_so) {
    Fixture fixture;
    const HealthAssessment assessment = fixture.run(kBase);
    AH_CHECK(assessment.state == HealthState::Unknown);
    AH_CHECK(assessment.flags.contains(AssessmentFlag::NoEvidence));
    AH_CHECK(has_finding(assessment, FindingCode::NoEvidenceForAsset));
    AH_CHECK(assessment.evidence_depended_on == 0);
    AH_CHECK(assessment.risk.band == RiskBand::Undetermined);
}

AH_TEST(evaluation, a_nil_request_is_refused_by_the_checked_entry_point) {
    EvaluationRequest request;
    EvaluationContext context;
    context.now = kBase;
    AH_REQUIRE_ERROR(error, evaluate_checked(request, HealthPolicy::standard(), context), ErrorCode::MalformedAssetReference);
}
