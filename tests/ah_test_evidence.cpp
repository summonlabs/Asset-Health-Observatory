// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Evidence admission and ordering: the boundary rules, the canonical order, the
// precedence rule, and the durable codec round trip.

#include <cstdint>
#include <string>
#include <vector>

#include "asset_health/evidence.hpp"
#include "store_format.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

Builder telemetry_builder(const char* source, SourceClass klass, std::uint64_t sequence) {
    Builder builder(asset_id(kAssetA), *SourceId::parse(source), SourceKind::TelemetryFeed, klass);
    return builder.sequence(sequence);
}

}  // namespace

AH_TEST(evidence, authority_domain_is_enforced_at_admission) {
    // A telemetry producer may not assert a lifecycle state: seeing the state is
    // not holding the authority that publishes it.
    Builder telemetry(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    auto parts = telemetry.lifecycle(LifecycleState::Active, std::nullopt);
    auto record = EvidenceRecord::make(parts);
    AH_REQUIRE_ERROR(error, record, ErrorCode::AuthorityDomainViolation);
    AH_CHECK(error.context().has_value());
    AH_CHECK(error.context()->second == "lifecycle");

    Builder registry(asset_id(kAssetA), *SourceId::parse("registry-a"), SourceKind::AssetRegistry,
                     SourceClass::Authoritative);
    auto identity = EvidenceRecord::make(registry.identity(3));
    AH_CHECK(identity.has_value());

    // And the other direction: the identity authority may not assert telemetry.
    auto reading = EvidenceRecord::make(registry.telemetry(MetricKind::Temperature, 40000, SampleQuality::Good));
    AH_REQUIRE_ERROR(other, reading, ErrorCode::AuthorityDomainViolation);
}

AH_TEST(evidence, synthetic_labelling_is_mandatory_and_symmetric) {
    Builder honest(asset_id(kAssetA), *SourceId::parse("plant-model"), SourceKind::SyntheticPlant,
                   SourceClass::Synthetic);
    AH_CHECK(EvidenceRecord::make(honest.telemetry(MetricKind::Temperature, 40000, SampleQuality::Good)).has_value());

    // A measured feed may not claim to be synthetic, and a synthetic producer may
    // not claim to be a measurement.
    Builder lying_feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed,
                       SourceClass::Synthetic);
    AH_REQUIRE_ERROR(first, EvidenceRecord::make(lying_feed.telemetry(MetricKind::Temperature, 40000, SampleQuality::Good)),
                     ErrorCode::SyntheticProvenanceRequired);

    Builder lying_model(asset_id(kAssetA), *SourceId::parse("plant-model"), SourceKind::SyntheticPlant,
                        SourceClass::Peer);
    AH_REQUIRE_ERROR(second,
                     EvidenceRecord::make(lying_model.telemetry(MetricKind::Temperature, 40000, SampleQuality::Good)),
                     ErrorCode::SyntheticProvenanceRequired);
}

AH_TEST(evidence, payload_consistency_is_checked_per_kind) {
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);

    auto wrong_unit = feed.telemetry(MetricKind::Temperature, 40000, SampleQuality::Good);
    wrong_unit.payload.telemetry.value = Quantity::make(Unit::Watts, 40000).value();
    AH_REQUIRE_ERROR(unit_error, EvidenceRecord::make(wrong_unit), ErrorCode::UnitMismatch);

    Builder registry(asset_id(kAssetA), *SourceId::parse("registry-a"), SourceKind::AssetRegistry,
                     SourceClass::Authoritative);
    auto no_revision = registry.identity(0);
    AH_REQUIRE_ERROR(revision_error, EvidenceRecord::make(no_revision), ErrorCode::InconsistentPayload);

    Builder maintenance(asset_id(kAssetA), *SourceId::parse("maint-a"), SourceKind::MaintenanceCoordinator,
                        SourceClass::Authoritative);
    const auto backwards = maintenance.maintenance(MaintenanceKind::Planned, MaintenanceState::Active,
                                                   instant("2026-02-01T00:00:00Z"), instant("2026-01-01T00:00:00Z"),
                                                   MaskScope::None);
    AH_REQUIRE_ERROR(window_error, EvidenceRecord::make(backwards), ErrorCode::InvalidTimeOrder);

    Builder faults(asset_id(kAssetA), *SourceId::parse("faults-a"), SourceKind::FaultFeed, SourceClass::Peer);
    auto empty_code = faults.fault("psu-1", "psu-ovt", FaultSeverity::Major, FaultStatus::Active);
    empty_code.payload.fault.code = FaultCode{};
    AH_REQUIRE_ERROR(code_error, EvidenceRecord::make(empty_code), ErrorCode::InconsistentPayload);

    Builder lifecycle(asset_id(kAssetA), *SourceId::parse("lifecycle-a"), SourceKind::HardwareLifecycle,
                      SourceClass::Authoritative);
    auto future_state = lifecycle.observed(instant("2026-01-01T00:00:00Z"))
                            .lifecycle(LifecycleState::Active, instant("2026-02-01T00:00:00Z"));
    AH_REQUIRE_ERROR(order_error, EvidenceRecord::make(future_state), ErrorCode::InvalidTimeOrder);
}

AH_TEST(evidence, fact_keys_group_statements_about_one_fact) {
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    const auto record = EvidenceRecord::make(feed.telemetry(MetricKind::Temperature, 40000, SampleQuality::Good));
    AH_REQUIRE(record.has_value());
    AH_CHECK(record->fact_key() == "telemetry:temperature");

    Builder faults(asset_id(kAssetA), *SourceId::parse("faults-a"), SourceKind::FaultFeed, SourceClass::Peer);
    const auto fault =
        EvidenceRecord::make(faults.fault("psu-1", "psu-ovt", FaultSeverity::Major, FaultStatus::Active));
    AH_REQUIRE(fault.has_value());
    AH_CHECK(fault->fact_key() == "fault:psu-1:psu-ovt");

    Builder registry(asset_id(kAssetA), *SourceId::parse("registry-a"), SourceKind::AssetRegistry,
                     SourceClass::Authoritative);
    AH_CHECK(EvidenceRecord::make(registry.identity(1))->fact_key() == "identity");

    AH_CHECK(EvidenceRecord::kinds_can_conflict(EvidenceKind::TelemetryReading, EvidenceKind::TelemetryReading));
    AH_CHECK(!EvidenceRecord::kinds_can_conflict(EvidenceKind::MaintenanceStatement,
                                                 EvidenceKind::MaintenanceStatement));
    AH_CHECK(!EvidenceRecord::kinds_can_conflict(EvidenceKind::TelemetryReading, EvidenceKind::FaultStatement));
}

AH_TEST(evidence, precedence_prefers_authority_then_recency) {
    const Instant base = instant("2026-01-01T00:00:00Z");
    Builder authoritative(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed,
                          SourceClass::Peer);
    Builder authoritative2(asset_id(kAssetA), *SourceId::parse("feed-b"), SourceKind::TelemetryFeed,
                           SourceClass::Peer);
    (void)authoritative;
    const auto older = EvidenceRecord::make(authoritative2.observed(base).telemetry(
        MetricKind::Temperature, 40000, SampleQuality::Good));
    const auto newer = EvidenceRecord::make(
        authoritative2.observed(*base.shifted(Duration::from_seconds(10))).telemetry(MetricKind::Temperature, 41000,
                                                                                     SampleQuality::Good));
    AH_REQUIRE(older.has_value());
    AH_REQUIRE(newer.has_value());
    AH_CHECK(EvidenceRecord::has_precedence_over(newer.value(), older.value()));
    AH_CHECK(!EvidenceRecord::has_precedence_over(older.value(), newer.value()));
}

AH_TEST(evidence, canonical_order_is_total_and_stable) {
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    Builder registry(asset_id(kAssetB), *SourceId::parse("registry-a"), SourceKind::AssetRegistry,
                     SourceClass::Authoritative);
    std::vector<EvidenceRecord> records;
    records.push_back(EvidenceRecord::make(feed.sequence(3).telemetry(MetricKind::Voltage, 12000, SampleQuality::Good)).value());
    records.push_back(EvidenceRecord::make(registry.sequence(1).identity(1)).value());
    records.push_back(EvidenceRecord::make(feed.sequence(1).telemetry(MetricKind::Temperature, 40000, SampleQuality::Good)).value());
    records.push_back(EvidenceRecord::make(feed.sequence(2).telemetry(MetricKind::Temperature, 41000, SampleQuality::Good)).value());

    const auto first = canonical_order(records);
    const auto second = canonical_order(records);
    AH_REQUIRE(first.size() == 4);
    AH_CHECK(first.front().subject().asset() == asset_id(kAssetA));
    AH_CHECK(first.back().subject().asset() == asset_id(kAssetB));
    for (std::size_t index = 0; index < first.size(); ++index) {
        AH_CHECK(first[index].id() == second[index].id());
    }
    for (std::size_t index = 1; index < first.size(); ++index) {
        AH_CHECK(EvidenceRecord::canonical_less(first[index - 1], first[index]));
    }
}

AH_TEST(evidence, durable_codec_round_trips_every_kind) {
    Builder registry(asset_id(kAssetA), *SourceId::parse("registry-a"), SourceKind::AssetRegistry,
                     SourceClass::Authoritative);
    Builder lifecycle(asset_id(kAssetA), *SourceId::parse("lifecycle-a"), SourceKind::HardwareLifecycle,
                      SourceClass::Authoritative);
    Builder maintenance(asset_id(kAssetA), *SourceId::parse("maint-a"), SourceKind::MaintenanceCoordinator,
                        SourceClass::Authoritative);
    Builder firmware(asset_id(kAssetA), *SourceId::parse("firmware-a"), SourceKind::FirmwareBaseline,
                     SourceClass::Authoritative);
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    Builder faults(asset_id(kAssetA), *SourceId::parse("faults-a"), SourceKind::FaultFeed, SourceClass::Peer);

    StoreState state;
    state.generation = GenerationSequence(4);
    state.epoch = StoreEpoch(2);
    state.evidence.push_back(EvidenceRecord::make(registry.identity(7)).value().with_commit_epoch(StoreEpoch(2)));
    state.evidence.push_back(
        EvidenceRecord::make(lifecycle.lifecycle(LifecycleState::Degraded, instant("2026-01-01T00:00:00Z")))
            .value()
            .with_commit_epoch(StoreEpoch(2)));
    state.evidence.push_back(
        EvidenceRecord::make(maintenance.maintenance(MaintenanceKind::Planned, MaintenanceState::Active,
                                                     instant("2026-01-01T00:00:00Z"), instant("2026-01-02T00:00:00Z"),
                                                     MaskScope::TelemetryAndFaults))
            .value()
            .with_commit_epoch(StoreEpoch(2)));
    state.evidence.push_back(EvidenceRecord::make(firmware.firmware("gpu-0", "2.1.0", "2.1.0", FirmwareCompliance::Matches))
                                 .value()
                                 .with_commit_epoch(StoreEpoch(2)));
    state.evidence.push_back(
        EvidenceRecord::make(feed.telemetry(MetricKind::Temperature, 42500, SampleQuality::Uncertain))
            .value()
            .with_commit_epoch(StoreEpoch(2)));
    state.evidence.push_back(
        EvidenceRecord::make(faults.fault("psu-1", "psu-ovt", FaultSeverity::Major, FaultStatus::Active))
            .value()
            .with_commit_epoch(StoreEpoch(2)));

    HistoryEntry entry;
    entry.asset = asset_id(kAssetA);
    entry.generation = AssetGeneration(1);
    entry.revision = AssessmentRevision(1);
    entry.state = HealthState::Degraded;
    entry.flags = AssessmentFlags::of(AssessmentFlag::StaleEvidence);
    entry.risk_band = RiskBand::Moderate;
    entry.evaluated_at = instant("2026-01-01T00:10:00Z");
    entry.recorded_at = instant("2026-01-01T00:10:01Z");
    entry.policy_fingerprint = "0123456789abcdef";
    entry.finding_count = 5;
    entry.attention_findings = 2;
    entry.evidence_depended_on = 6;
    state.history.push_back(entry);

    RefusalRecord refusal;
    refusal.asset = asset_id(kAssetB);
    refusal.generation = AssetGeneration(1);
    refusal.source = *SourceId::parse("feed-z");
    refusal.source_kind = SourceKind::TelemetryFeed;
    refusal.claimed_kind = EvidenceKind::LifecycleStatement;
    refusal.code = ErrorCode::AuthorityDomainViolation;
    refusal.detail = "the source does not speak for the domain";
    refusal.received_at = instant("2026-01-01T00:00:00Z");
    state.refusals.push_back(refusal);

    SourceStreamState stream;
    stream.source = *SourceId::parse("feed-a");
    stream.kind = SourceKind::TelemetryFeed;
    stream.epoch = StreamEpoch(3);
    stream.last_sequence = StreamSequence(11);
    stream.last_digest = 0xABCDEF0123456789ull;
    stream.last_observed_at = instant("2026-01-01T00:00:00Z");
    stream.last_received_at = instant("2026-01-01T00:00:01Z");
    stream.last_evidence_id = EvidenceId::generate().value();
    state.source_streams.push_back(stream);

    RetryRecord retry;
    retry.sequence = GenerationSequence(4);
    retry.idempotency_key = "ingest:feed-a:1:11";
    retry.resulting_generation = GenerationSequence(4);
    retry.committed_at = instant("2026-01-01T00:00:01Z");
    state.retries.push_back(retry);

    detail::GenerationHeader header;
    header.generation = 4;
    header.epoch = 2;
    header.store_id = *parse_uuid_text("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    header.previous_generation = 3;
    header.previous_payload_digest = 12345;
    header.pruned_below = 1;
    const std::vector<std::uint8_t> file = detail::encode_generation(state, header);

    StoreState decoded;
    detail::GenerationHeader decoded_header;
    const Status status = detail::decode_generation(file, decoded, decoded_header);
    AH_REQUIRE_MSG(status.has_value(), status.error().to_string());
    AH_CHECK(decoded.evidence.size() == 6);
    AH_CHECK(decoded.history.size() == 1);
    AH_CHECK(decoded.refusals.size() == 1);
    AH_CHECK(decoded.source_streams.size() == 1);
    AH_CHECK(decoded.retries.size() == 1);
    AH_CHECK(decoded.evidence[0].id() == state.evidence[0].id());
    AH_CHECK(decoded.evidence[4].payload().telemetry.value.to_string() == "42.500 c");
    AH_CHECK(decoded.evidence[4].payload().telemetry.quality == SampleQuality::Uncertain);
    AH_CHECK(decoded.evidence[5].payload().fault.code.str() == "psu-ovt");
    AH_CHECK(decoded.evidence[1].payload().lifecycle.state == LifecycleState::Degraded);
    AH_CHECK(decoded.source_streams[0].last_evidence_id == stream.last_evidence_id);
    AH_CHECK(decoded.history[0].state == HealthState::Degraded);
    AH_CHECK(decoded.refusals[0].code == ErrorCode::AuthorityDomainViolation);

    // A single flipped bit in the payload is caught by the checksum.
    std::vector<std::uint8_t> damaged = file;
    damaged[damaged.size() - 20] ^= 0x01;
    StoreState ignored;
    detail::GenerationHeader ignored_header;
    const Status damaged_status = detail::decode_generation(damaged, ignored, ignored_header);
    AH_CHECK(!damaged_status.has_value());
    AH_CHECK(damaged_status.error().code() == ErrorCode::PayloadChecksumMismatch);

    // A file that stops early is a torn tail, which is a different answer.
    std::vector<std::uint8_t> torn(file.begin(), file.end() - 4);
    const Status torn_status = detail::decode_generation(torn, ignored, ignored_header);
    AH_CHECK(!torn_status.has_value());
    AH_CHECK(torn_status.error().code() == ErrorCode::TornTailDiscarded);

    // A header whose own checksum fails is refused before the payload is read.
    std::vector<std::uint8_t> bad_header = file;
    bad_header[10] ^= 0xFF;
    const Status header_status = detail::decode_generation(bad_header, ignored, ignored_header);
    AH_CHECK(!header_status.has_value());
    AH_CHECK(header_status.error().code() == ErrorCode::HeaderChecksumMismatch);
}

AH_TEST(evidence, unknown_names_in_a_durable_record_are_corruption) {
    // A payload that decodes but names a metric outside the vocabulary must be
    // refused rather than mapped onto a neighbouring value.
    StoreState state;
    state.generation = GenerationSequence(1);
    detail::GenerationHeader header;
    header.generation = 1;
    header.store_id = *parse_uuid_text("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const std::vector<std::uint8_t> file = detail::encode_generation(state, header);
    std::vector<std::uint8_t> damaged = file;
    // Overwrite the last payload byte, which is inside the trailing stream count
    // in an empty state, and re-checksum it so that the record decoder is what
    // rejects the file rather than the checksum.
    damaged[damaged.size() - 1] = 0x7F;
    StoreState decoded;
    detail::GenerationHeader decoded_header;
    const Status status = detail::decode_generation(damaged, decoded, decoded_header);
    AH_CHECK(!status.has_value());
}
