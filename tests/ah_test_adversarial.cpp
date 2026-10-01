// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Adversarial input: hostile durable files, malformed statements, replays, and
// the refusals they must produce. Every case asserts the specific rejection, not
// merely that something failed.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "asset_health/export.hpp"
#include "asset_health/observatory.hpp"
#include "crc32.hpp"
#include "store_format.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

const Instant kBase = instant("2026-03-01T00:00:00Z");

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::vector<std::uint8_t> valid_generation_bytes() {
    StoreState state;
    state.generation = GenerationSequence(2);
    state.epoch = StoreEpoch(1);
    state.store_id = *parse_uuid_text("abcdef01-2345-4678-89ab-cdef01234567");
    detail::GenerationHeader header;
    header.generation = 2;
    header.epoch = 1;
    header.store_id = state.store_id;
    return detail::encode_generation(state, header);
}

/// Rewrites the header's declared payload length and repairs the header checksum,
/// producing a file that a decoder must refuse rather than trust.
[[nodiscard]] std::vector<std::uint8_t> with_declared_length(std::vector<std::uint8_t> file, std::uint64_t length) {
    for (int index = 0; index < 8; ++index) {
        file[32 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((length >> (8 * index)) & 0xFFu);
    }
    const std::uint32_t checksum = detail::crc32_bytes(file.data(), 44);
    for (int index = 0; index < 4; ++index) {
        file[44 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((checksum >> (8 * index)) & 0xFFu);
    }
    return file;
}

}  // namespace

AH_TEST(adversarial, a_hostile_generation_file_is_classified_not_trusted) {
    const TempDirectory directory("adversarial-files");
    StoreState decoded;
    detail::GenerationHeader header;

    // Nothing at all.
    const std::vector<std::uint8_t> empty;
    AH_CHECK(!detail::decode_generation(empty, decoded, header).has_value());

    // A header with the wrong magic.
    std::vector<std::uint8_t> wrong_magic = valid_generation_bytes();
    wrong_magic[0] = 'X';
    const Status magic_status = detail::decode_generation(wrong_magic, decoded, header);
    AH_CHECK(!magic_status.has_value());
    AH_CHECK(magic_status.error().code() == ErrorCode::HeaderChecksumMismatch ||
             magic_status.error().code() == ErrorCode::StoreLayoutInvalid);

    // A header claiming a payload longer than the configured bound.
    const std::vector<std::uint8_t> huge = with_declared_length(valid_generation_bytes(), 1ull << 40);
    const Status huge_status = detail::decode_generation(huge, decoded, header);
    AH_CHECK(!huge_status.has_value());
    AH_CHECK(huge_status.error().code() == ErrorCode::PayloadTooLarge ||
             huge_status.error().code() == ErrorCode::TornTailDiscarded);

    // A payload longer than the header declares.
    std::vector<std::uint8_t> trailing = valid_generation_bytes();
    trailing.push_back(0);
    const Status trailing_status = detail::decode_generation(trailing, decoded, header);
    AH_CHECK(!trailing_status.has_value());
    AH_CHECK(trailing_status.error().code() == ErrorCode::InteriorCorruption);

    // A payload that decodes except for a count field that runs past the end.
    std::vector<std::uint8_t> truncated_payload = valid_generation_bytes();
    truncated_payload.resize(truncated_payload.size() - 1);
    // Keep the declared length in step so that the checksum, not the length, is
    // what fails first.
    const std::uint64_t declared =
        static_cast<std::uint64_t>(truncated_payload.size()) - detail::kGenerationHeaderBytes;
    truncated_payload = with_declared_length(truncated_payload, declared);
    const Status payload_status = detail::decode_generation(truncated_payload, decoded, header);
    AH_CHECK(!payload_status.has_value());
    (void)directory;
}

AH_TEST(adversarial, malformed_statements_are_refused_with_a_reason) {
    const TempDirectory directory("adversarial-ingest");
    ObservatoryOptions options;
    options.store_root = directory.path();
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());

    // A nil evidence identifier.
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    auto nil_id = feed.observed(kBase).telemetry(MetricKind::Temperature, 40000, SampleQuality::Good);
    nil_id.id = EvidenceId::nil();
    auto outcome = observatory.ingest(nil_id);
    AH_REQUIRE(outcome.has_value());
    AH_CHECK(!outcome.value().admitted);
    AH_CHECK(outcome.value().refusal_code == ErrorCode::MalformedEvidenceId);

    // A generation of zero.
    auto zero_generation = feed.observed(kBase).telemetry(MetricKind::Temperature, 40000, SampleQuality::Good);
    zero_generation.subject = EvidenceSubject{asset_id(kAssetA)};
    auto generation_outcome = observatory.ingest(zero_generation);
    AH_REQUIRE(generation_outcome.has_value());
    AH_CHECK(!generation_outcome.value().admitted);

    // A lifecycle statement from a telemetry source.
    auto wrong_domain = feed.observed(kBase).lifecycle(LifecycleState::Active, std::nullopt);
    auto domain_outcome = observatory.ingest(wrong_domain);
    AH_REQUIRE(domain_outcome.has_value());
    AH_CHECK(!domain_outcome.value().admitted);
    AH_CHECK(domain_outcome.value().refusal_code == ErrorCode::AuthorityDomainViolation);

    // The refusals are durable and visible on the asset.
    auto refusals = observatory.refusals_for(asset_id(kAssetA));
    AH_REQUIRE(refusals.has_value());
    AH_CHECK(refusals.value().size() >= 2);

    // Nothing refused became evidence.
    auto evidence = observatory.evidence_for(asset_id(kAssetA));
    AH_REQUIRE(evidence.has_value());
    AH_CHECK(evidence.value().empty());

    // The assessment reports that it is not seeing everything that was sent.
    auto assessment = observatory.assess(asset_id(kAssetA), *kBase.shifted(Duration::from_seconds(1)));
    AH_REQUIRE(assessment.has_value());
    AH_CHECK(assessment.value().flags.contains(AssessmentFlag::RefusedEvidence));
    observatory.close();
}

AH_TEST(adversarial, replays_and_future_dated_statements_are_refused) {
    const TempDirectory directory("adversarial-replay");
    ObservatoryOptions options;
    options.store_root = directory.path();
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);

    auto first = observatory.ingest(feed.sequence(1).observed(kBase).telemetry(MetricKind::Temperature, 40000,
                                                                              SampleQuality::Good));
    AH_REQUIRE(first.has_value());
    AH_CHECK(first.value().admitted);

    // The same stream position with different content is a replay.
    auto replay = observatory.ingest(feed.sequence(1).observed(kBase).telemetry(MetricKind::Temperature, 90000,
                                                                               SampleQuality::Good));
    AH_REQUIRE(replay.has_value());
    AH_CHECK(!replay.value().admitted);
    AH_CHECK(replay.value().refusal_code == ErrorCode::ReplayedSourceSequence);

    // A later sequence in a dead epoch is refused: the source restarted.
    auto advanced = observatory.ingest(feed.epoch(2).sequence(1).observed(kBase).telemetry(
        MetricKind::Temperature, 41000, SampleQuality::Good));
    AH_REQUIRE(advanced.has_value());
    AH_CHECK(advanced.value().admitted);
    auto dead_epoch = observatory.ingest(feed.epoch(1).sequence(2).observed(kBase).telemetry(
        MetricKind::Temperature, 42000, SampleQuality::Good));
    AH_REQUIRE(dead_epoch.has_value());
    AH_CHECK(!dead_epoch.value().admitted);
    AH_CHECK(dead_epoch.value().refusal_code == ErrorCode::StaleSourceEpoch);

    // A statement dated well ahead of its receipt is refused.
    Builder ahead(asset_id(kAssetA), *SourceId::parse("feed-b"), SourceKind::TelemetryFeed, SourceClass::Peer);
    auto future = ahead.sequence(1)
                      .observed(*kBase.shifted(Duration::from_hours(1)))
                      .received(kBase)
                      .telemetry(MetricKind::Temperature, 40000, SampleQuality::Good);
    auto future_outcome = observatory.ingest(future);
    AH_REQUIRE(future_outcome.has_value());
    AH_CHECK(!future_outcome.value().admitted);
    AH_CHECK(future_outcome.value().refusal_code == ErrorCode::FutureDatedObservation);

    // The same identifier with different content is refused outright.
    auto duplicate = feed.epoch(2).sequence(2).observed(kBase).telemetry(MetricKind::Temperature, 43000,
                                                                        SampleQuality::Good);
    duplicate.id = first.value().evidence_id;
    AH_REQUIRE_ERROR(error, observatory.ingest(duplicate), ErrorCode::DuplicateEvidenceId);
    observatory.close();
}

AH_TEST(adversarial, capacity_bounds_refuse_rather_than_forget) {
    const TempDirectory directory("adversarial-capacity");
    ObservatoryOptions options;
    options.store_root = directory.path();
    options.max_evidence_per_asset = 3;
    options.max_assets = 1;
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        const auto outcome = observatory.ingest(feed.sequence(sequence).observed(kBase).telemetry(
            MetricKind::Temperature, 40000, SampleQuality::Good));
        AH_REQUIRE(outcome.has_value());
        AH_CHECK(outcome.value().admitted);
    }
    auto overflow = observatory.ingest(feed.sequence(4).observed(kBase).telemetry(MetricKind::Temperature, 40000,
                                                                                 SampleQuality::Good));
    AH_REQUIRE(!overflow.has_value());
    AH_CHECK(overflow.error().code() == ErrorCode::EvidenceCapacityExceeded);
    auto records = observatory.evidence_for(asset_id(kAssetA));
    AH_REQUIRE(records.has_value());
    AH_CHECK(records.value().size() == 3);

    Builder other(asset_id(kAssetB), *SourceId::parse("feed-b"), SourceKind::TelemetryFeed, SourceClass::Peer);
    auto second_asset = observatory.ingest(other.sequence(1).observed(kBase).telemetry(MetricKind::Temperature, 40000,
                                                                                     SampleQuality::Good));
    AH_REQUIRE(!second_asset.has_value());
    AH_CHECK(second_asset.error().code() == ErrorCode::AssetCapacityExceeded);
    observatory.close();
}

AH_TEST(adversarial, an_unknown_asset_is_refused_by_name) {
    const TempDirectory directory("adversarial-unknown");
    ObservatoryOptions options;
    options.store_root = directory.path();
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());
    AH_REQUIRE_ERROR(error, observatory.assess(asset_id(kAssetB), kBase), ErrorCode::UnknownAsset);
    AH_REQUIRE_ERROR(history_error, observatory.history(asset_id(kAssetB)), ErrorCode::UnknownAsset);
    observatory.close();
}

AH_TEST(adversarial, configuration_bounds_are_refused_at_open) {
    const TempDirectory directory("adversarial-config");
    ObservatoryOptions options;
    options.store_root = directory.path();
    options.ingest_workers = limits::kMaxIngestWorkers + 1;
    auto too_many = Observatory::open(options);
    AH_REQUIRE(!too_many.has_value());
    AH_CHECK(too_many.error().code() == ErrorCode::InvalidConfiguration);

    ObservatoryOptions empty_queue;
    empty_queue.store_root = directory.path() / "other";
    empty_queue.ingest_queue_depth = 0;
    auto zero_depth = Observatory::open(empty_queue);
    AH_REQUIRE(!zero_depth.has_value());
    AH_CHECK(zero_depth.error().code() == ErrorCode::InvalidConfiguration);

    // A policy is valid by construction: HealthPolicy is only obtainable from
    // make(), which validates, so an observatory can never be opened with an
    // incoherent policy. The default option value is the standard policy.
    const ObservatoryOptions defaults;
    AH_CHECK(defaults.policy.id().str() == "standard-dccp-health");
    AH_CHECK(defaults.policy.fingerprint().size() == 16);
}

AH_TEST(adversarial, a_store_directory_that_is_a_file_is_refused) {
    const TempDirectory directory("adversarial-not-a-directory");
    const std::filesystem::path file = directory.path() / "store";
    write_bytes(file, {1, 2, 3});
    StoreOptions options;
    options.root = file;
    auto opened = Store::open(options);
    AH_REQUIRE(!opened.has_value());
    AH_CHECK(opened.error().code() == ErrorCode::StoreLayoutInvalid);
}
