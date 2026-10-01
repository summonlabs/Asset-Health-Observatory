// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Freshness is the rule that decides whether an admitted statement still counts.
// Every branch of it is pinned here, because a defect in this rule is a defect in
// every answer the observatory gives.

#include <cstdint>
#include <map>
#include <string>

#include "asset_health/freshness.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

const FreshnessPolicy kPolicy = [] {
    FreshnessPolicy policy;
    policy.dynamic_window = Duration::from_seconds(300);
    policy.static_window = Duration::from_days(30);
    policy.future_tolerance = Duration::from_seconds(5);
    policy.static_unaged = false;
    return policy;
}();

std::map<SourceId, StreamEpoch> no_streams() { return {}; }

/// Judges a record, defaulting to an orderly restart. The recovery flag is the
/// rule under test in one case, so it is the one thing every other case states
/// implicitly.
FreshnessResult judge(const EvidenceRecord& record, Instant now, const FreshnessPolicy& policy, StoreEpoch epoch,
                      const std::map<SourceId, StreamEpoch>& streams, bool recovered = false) {
    return evaluate_freshness(record, now, policy, epoch, streams, recovered);
}

}  // namespace

AH_TEST(freshness, dynamic_evidence_is_fresh_inside_its_window_and_stale_after_it) {
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    const Instant base = instant("2026-01-01T00:00:00Z");
    const auto record = EvidenceRecord::make(feed.observed(base).telemetry(MetricKind::Temperature, 40000,
                                                                          SampleQuality::Good))
                            .value()
                            .with_commit_epoch(StoreEpoch(1));

    const auto fresh = judge(record, *base.shifted(Duration::from_seconds(299)), kPolicy, StoreEpoch(1),
                                          no_streams());
    AH_CHECK(fresh.state == Freshness::Fresh);
    AH_CHECK(fresh.usable);
    AH_CHECK(fresh.reason == FreshnessReason::WithinWindow);

    const auto boundary = judge(record, *base.shifted(Duration::from_seconds(300)), kPolicy, StoreEpoch(1),
                                             no_streams());
    AH_CHECK(boundary.state == Freshness::Fresh);

    const auto stale = judge(record, *base.shifted(Duration::from_seconds(301)), kPolicy, StoreEpoch(1),
                                          no_streams());
    AH_CHECK(stale.state == Freshness::Stale);
    AH_CHECK(!stale.usable);
    AH_CHECK(stale.reason == FreshnessReason::AgeExceedsWindow);
    AH_CHECK(!stale.explanation.empty());
}

AH_TEST(freshness, recovered_dynamic_evidence_is_not_fresh_evidence) {
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    const Instant base = instant("2026-01-01T00:00:00Z");
    const auto record = EvidenceRecord::make(feed.observed(base).telemetry(MetricKind::Temperature, 40000,
                                                                          SampleQuality::Good))
                            .value()
                            .with_commit_epoch(StoreEpoch(1));

    // The store reopened and published epoch 2, so the record was committed in an
    // earlier epoch. After an orderly restart it is still judged by its age: the
    // store's history is what it was written to be.
    const auto orderly = judge(record, *base.shifted(Duration::from_seconds(1)), kPolicy, StoreEpoch(2),
                               no_streams());
    AH_CHECK(orderly.state == Freshness::Fresh);
    AH_CHECK(orderly.usable);

    // After a session that died, the same reading is recovered, and a recovered
    // reading is not current even when it is only a second old.
    const auto recovered = judge(record, *base.shifted(Duration::from_seconds(1)), kPolicy, StoreEpoch(2),
                                 no_streams(), true);
    AH_CHECK(recovered.state == Freshness::Recovered);
    AH_CHECK(!recovered.usable);
    AH_CHECK(recovered.reason == FreshnessReason::InPreviousStoreEpoch);

    // Static evidence is a state, not a reading, so it survives the epoch change.
    Builder lifecycle(asset_id(kAssetA), *SourceId::parse("lifecycle-a"), SourceKind::HardwareLifecycle,
                      SourceClass::Authoritative);
    const auto static_record = EvidenceRecord::make(lifecycle.observed(base).lifecycle(LifecycleState::Active,
                                                                                       std::nullopt))
                                   .value()
                                   .with_commit_epoch(StoreEpoch(1));
    const auto still_usable = judge(static_record, *base.shifted(Duration::from_seconds(1)), kPolicy,
                                    StoreEpoch(2), no_streams(), true);
    AH_CHECK(still_usable.state == Freshness::Fresh);
    AH_CHECK(still_usable.usable);
}

AH_TEST(freshness, a_restarted_stream_supersedes_its_own_past) {
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    const Instant base = instant("2026-01-01T00:00:00Z");
    const auto record = EvidenceRecord::make(feed.epoch(1).observed(base).telemetry(MetricKind::Temperature, 40000,
                                                                                   SampleQuality::Good))
                            .value()
                            .with_commit_epoch(StoreEpoch(1));

    std::map<SourceId, StreamEpoch> streams;
    streams[*SourceId::parse("feed-a")] = StreamEpoch(2);
    const auto superseded = judge(record, *base.shifted(Duration::from_seconds(1)), kPolicy, StoreEpoch(1),
                                               streams);
    AH_CHECK(superseded.state == Freshness::SupersededEpoch);
    AH_CHECK(!superseded.usable);
    AH_CHECK(superseded.reason == FreshnessReason::SourceEpochSuperseded);

    std::map<SourceId, StreamEpoch> current;
    current[*SourceId::parse("feed-a")] = StreamEpoch(1);
    AH_CHECK(judge(record, *base.shifted(Duration::from_seconds(1)), kPolicy, StoreEpoch(1), current)
                 .usable);
}

AH_TEST(freshness, a_statement_dated_ahead_of_the_evaluation_is_refused) {
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    const Instant base = instant("2026-01-01T00:00:00Z");
    const auto record = EvidenceRecord::make(feed.observed(base).telemetry(MetricKind::Temperature, 40000,
                                                                          SampleQuality::Good))
                            .value()
                            .with_commit_epoch(StoreEpoch(1));

    const auto within_tolerance = judge(record, *base.shifted(Duration::from_seconds(-5)), kPolicy,
                                                     StoreEpoch(1), no_streams());
    AH_CHECK(within_tolerance.state == Freshness::Fresh);

    const auto ahead = judge(record, *base.shifted(Duration::from_seconds(-6)), kPolicy, StoreEpoch(1),
                                          no_streams());
    AH_CHECK(ahead.state == Freshness::FutureDated);
    AH_CHECK(!ahead.usable);
    AH_CHECK(ahead.reason == FreshnessReason::ObservedInFuture);
}

AH_TEST(freshness, static_evidence_is_aged_unless_the_policy_says_otherwise) {
    Builder lifecycle(asset_id(kAssetA), *SourceId::parse("lifecycle-a"), SourceKind::HardwareLifecycle,
                      SourceClass::Authoritative);
    const Instant base = instant("2026-01-01T00:00:00Z");
    const auto record = EvidenceRecord::make(lifecycle.observed(base).lifecycle(LifecycleState::Active, std::nullopt))
                            .value()
                            .with_commit_epoch(StoreEpoch(1));

    const auto aged = judge(record, *base.shifted(Duration::from_days(31)), kPolicy, StoreEpoch(1),
                                         no_streams());
    AH_CHECK(aged.state == Freshness::Stale);
    AH_CHECK(aged.reason == FreshnessReason::StaticStateAged);

    FreshnessPolicy unaged_policy = kPolicy;
    unaged_policy.static_unaged = true;
    const auto unaged = judge(record, *base.shifted(Duration::from_days(3100)), unaged_policy,
                                           StoreEpoch(1), no_streams());
    AH_CHECK(unaged.state == Freshness::Unaged);
    AH_CHECK(unaged.usable);
    AH_CHECK(unaged.reason == FreshnessReason::StaticStateNotAged);
}

AH_TEST(freshness, a_zero_window_means_nothing_is_ever_fresh) {
    FreshnessPolicy zero = kPolicy;
    zero.dynamic_window = Duration::from_seconds(0);
    Builder feed(asset_id(kAssetA), *SourceId::parse("feed-a"), SourceKind::TelemetryFeed, SourceClass::Peer);
    const Instant base = instant("2026-01-01T00:00:00Z");
    const auto record = EvidenceRecord::make(feed.observed(base).telemetry(MetricKind::Temperature, 40000,
                                                                          SampleQuality::Good))
                            .value()
                            .with_commit_epoch(StoreEpoch(1));
    AH_CHECK(judge(record, base, zero, StoreEpoch(1), no_streams()).state == Freshness::Fresh);
    AH_CHECK(judge(record, *base.shifted(Duration::from_nanoseconds(1)), zero, StoreEpoch(1),
                                no_streams())
                 .state == Freshness::Stale);
}
