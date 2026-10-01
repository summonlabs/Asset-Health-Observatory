// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Properties, over seeded random input. Every case prints its seed when it
// fails, and re-running with that seed reproduces the same sequence, so a
// failure here is a reproducible defect rather than a coincidence.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "asset_health/evaluation.hpp"
#include "asset_health/export.hpp"
#include "store_format.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

const Instant kBase = instant("2026-02-01T00:00:00Z");

[[nodiscard]] bool has_finding(const HealthAssessment& assessment, FindingCode code) {
    return std::any_of(assessment.findings.begin(), assessment.findings.end(),
                       [code](const Finding& finding) { return finding.code == code; });
}

[[nodiscard]] EvidenceRecord random_reading(Random& random, std::uint64_t sequence, Instant observed) {
    Builder feed(asset_id(kAssetA),
                 *SourceId::parse(random.below(2) == 0 ? "feed-a" : "feed-b"), SourceKind::TelemetryFeed,
                 SourceClass::Peer);
    const auto qualities = {SampleQuality::Good, SampleQuality::Good, SampleQuality::Uncertain,
                            SampleQuality::Substituted};
    const MetricKind metrics[] = {MetricKind::Temperature, MetricKind::PowerDraw, MetricKind::FanSpeed};
    const MetricKind metric = metrics[random.below(3)];
    const std::int64_t milli = random.range(0, 100000);
    const SampleQuality quality = *(qualities.begin() + static_cast<std::ptrdiff_t>(random.below(4)));
    return EvidenceRecord::make(feed.epoch(1)
                                    .sequence(sequence)
                                    .observed(observed)
                                    .telemetry(metric, milli, quality, random.below(4) == 0))
        .value()
        .with_commit_epoch(StoreEpoch(1));
}

}  // namespace

AH_TEST(property, evaluation_is_a_function_of_its_inputs) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Random random(seed);
        std::vector<EvidenceRecord> records;
        const std::uint64_t count = 1 + random.below(12);
        for (std::uint64_t index = 0; index < count; ++index) {
            const Instant observed = *kBase.shifted(Duration::from_seconds(random.range(0, 120)));
            records.push_back(random_reading(random, index + 1, observed));
        }
        EvaluationRequest request;
        request.asset = asset_id(kAssetA);
        request.generation = AssetGeneration(1);
        request.evidence = records;
        EvaluationContext context;
        context.now = *kBase.shifted(Duration::from_seconds(150));
        context.store_epoch = StoreEpoch(1);
        const HealthAssessment first = evaluate(request, HealthPolicy::standard(), context);
        const HealthAssessment second = evaluate(request, HealthPolicy::standard(), context);
        AH_CHECK_MSG(first.explain() == second.explain(), "seed " + std::to_string(seed));

        // Shuffling the input must not change the answer: the evaluator sorts the
        // evidence into its own canonical order.
        std::vector<EvidenceRecord> shuffled = records;
        for (std::size_t index = shuffled.size(); index > 1; --index) {
            const std::size_t other = static_cast<std::size_t>(random.below(index));
            std::swap(shuffled[index - 1], shuffled[other]);
        }
        request.evidence = shuffled;
        const HealthAssessment third = evaluate(request, HealthPolicy::standard(), context);
        AH_CHECK_MSG(third.explain() == first.explain(), "seed " + std::to_string(seed));
    }
}

AH_TEST(property, the_durable_codec_round_trips_random_states) {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        Random random(seed);
        StoreState state;
        state.generation = GenerationSequence(1 + random.below(1000));
        state.epoch = StoreEpoch(1 + random.below(50));
        state.store_id = *parse_uuid_text("abcdef01-2345-4678-89ab-cdef01234567");
        const std::uint64_t count = random.below(24);
        for (std::uint64_t index = 0; index < count; ++index) {
            const Instant observed = *kBase.shifted(Duration::from_seconds(random.range(0, 10000)));
            state.evidence.push_back(random_reading(random, index + 1, observed));
        }
        std::sort(state.evidence.begin(), state.evidence.end(),
                  [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
                      return EvidenceRecord::canonical_less(lhs, rhs);
                  });

        detail::GenerationHeader header;
        header.generation = state.generation.value();
        header.epoch = state.epoch.value();
        header.store_id = state.store_id;
        const std::vector<std::uint8_t> file = detail::encode_generation(state, header);
        StoreState decoded;
        detail::GenerationHeader decoded_header;
        const Status status = detail::decode_generation(file, decoded, decoded_header);
        AH_REQUIRE_MSG(status.has_value(), "seed " + std::to_string(seed) + ": " + status.error().to_string());
        AH_CHECK(decoded.evidence.size() == state.evidence.size());
        for (std::size_t index = 0; index < state.evidence.size(); ++index) {
            AH_CHECK_MSG(decoded.evidence[index].id() == state.evidence[index].id(),
                         "seed " + std::to_string(seed) + " index " + std::to_string(index));
            AH_CHECK(detail::evidence_digest(decoded.evidence[index]) ==
                     detail::evidence_digest(state.evidence[index]));
        }
    }
}

AH_TEST(property, values_round_trip_through_their_canonical_text) {
    Random random(20260101);
    for (int index = 0; index < 500; ++index) {
        const std::int64_t milli = random.range(-1000000000LL, 1000000000LL);
        const Quantity quantity = *Quantity::make(Unit::Celsius, milli);
        const auto parsed = Quantity::parse(quantity.to_string());
        AH_REQUIRE(parsed.has_value());
        AH_CHECK(parsed->milli() == milli);

        const std::int64_t nanos = random.range(0, 4000000000000000000LL);
        const Instant instant_value(nanos);
        const auto reparsed = parse_instant(instant_value.to_string());
        AH_REQUIRE(reparsed.has_value());
        AH_CHECK(*reparsed == instant_value);
    }
}

AH_TEST(property, rational_arithmetic_is_reversible_where_it_is_representable) {
    Random random(4242);
    for (int index = 0; index < 500; ++index) {
        const auto left = Rational::make(random.range(-100000, 100000), random.range(1, 100000));
        const auto right = Rational::make(random.range(-100000, 100000), random.range(1, 100000));
        AH_REQUIRE(left.has_value());
        AH_REQUIRE(right.has_value());
        const auto sum = left->add(*right);
        if (!sum.has_value()) {
            continue;
        }
        // A checked operation may refuse when the exact result does not fit. The
        // property is that a representable result is exact, not that every
        // intermediate is representable.
        const auto back = sum->subtract(*right);
        if (!back.has_value()) {
            continue;
        }
        AH_CHECK(*back == *left);
        AH_CHECK(left->compare(*right) == -right->compare(*left));
    }
}

AH_TEST(property, an_observatory_holds_exactly_what_it_admitted) {
    const TempDirectory directory("property-store");
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        Random random(seed);
        ObservatoryOptions options;
        options.store_root = directory.path() / ("store-" + std::to_string(seed));
        auto opened = Observatory::open(options);
        AH_REQUIRE(opened.has_value());
        Observatory observatory = std::move(opened.value());
        std::vector<EvidenceId> admitted;
        const std::uint64_t count = 5 + random.below(30);
        for (std::uint64_t index = 0; index < count; ++index) {
            const Instant observed = *kBase.shifted(Duration::from_seconds(random.range(0, 60)));
            // Sequences advance inside one stream, so every statement is a new
            // position and none is a replay.
            Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed,
                         SourceClass::Peer);
            const auto parts = feed.epoch(1)
                                   .sequence(index + 1)
                                   .observed(observed)
                                   .telemetry(MetricKind::Temperature, random.range(0, 100000), SampleQuality::Good);
            auto outcome = observatory.ingest(parts);
            AH_REQUIRE_MSG(outcome.has_value(), "seed " + std::to_string(seed));
            AH_REQUIRE(outcome.value().admitted);
            admitted.push_back(outcome.value().evidence_id);
        }
        auto records = observatory.evidence_for(asset_id(kAssetA));
        AH_REQUIRE(records.has_value());
        AH_CHECK(records.value().size() == admitted.size());
        for (std::size_t index = 0; index < records.value().size(); ++index) {
            AH_CHECK(std::find(admitted.begin(), admitted.end(), records.value()[index].id()) != admitted.end());
        }
        for (std::size_t index = 1; index < records.value().size(); ++index) {
            AH_CHECK(EvidenceRecord::canonical_less(records.value()[index - 1], records.value()[index]));
        }
        observatory.close();
    }
}

AH_TEST(property, a_reopened_store_reports_the_same_evidence) {
    const TempDirectory directory("property-reopen");
    ObservatoryOptions options;
    options.store_root = directory.path();
    std::vector<std::string> before;
    {
        auto opened = Observatory::open(options);
        AH_REQUIRE(opened.has_value());
        Observatory observatory = std::move(opened.value());
        Random random(7);
        for (std::uint64_t index = 0; index < 12; ++index) {
            Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed,
                         SourceClass::Peer);
            const auto observed = *kBase.shifted(Duration::from_seconds(static_cast<std::int64_t>(index)));
            const auto parts = feed.sequence(index + 1).observed(observed).telemetry(
                MetricKind::Temperature, random.range(0, 100000), SampleQuality::Good);
            AH_REQUIRE(observatory.ingest(parts).has_value());
        }
        auto records = observatory.evidence_for(asset_id(kAssetA));
        AH_REQUIRE(records.has_value());
        for (const EvidenceRecord& record : records.value()) {
            before.push_back(to_json(record));
        }
        observatory.close();
    }
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());
    auto records = observatory.evidence_for(asset_id(kAssetA));
    AH_REQUIRE(records.has_value());
    AH_REQUIRE(records.value().size() == before.size());
    for (std::size_t index = 0; index < records.value().size(); ++index) {
        AH_CHECK(to_json(records.value()[index]) == before[index]);
    }
    // The store was closed cleanly, so the readings are still judged by their
    // age: the assessment does not declare them recovered, and the answer is the
    // one the evidence supports.
    const HealthAssessment assessment = observatory.assess(asset_id(kAssetA),
                                                           *kBase.shifted(Duration::from_seconds(20))).value();
    AH_CHECK(!assessment.flags.contains(AssessmentFlag::StaleEvidence));
    AH_CHECK(!has_finding(assessment, FindingCode::MetricRecovered));
    AH_CHECK(assessment.state == HealthState::Unknown || assessment.state == HealthState::Healthy);
    observatory.close();
}
