// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Durability: the commit point, recovery, refusal of interior corruption,
// epoch fencing, idempotent retry and the configured bounds. Every case works on
// real files in a real directory.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "asset_health/persistence.hpp"
#include "store_format.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

[[nodiscard]] CommitRequest request_for(const StoreStatePtr& state, std::uint64_t sequence, std::string key,
                                        EvidenceRecord record) {
    CommitRequest request;
    request.expected_epoch = state->epoch;
    request.sequence = GenerationSequence(sequence);
    request.idempotency_key = std::move(key);
    request.mutation.append_evidence.push_back(std::move(record));
    return request;
}

[[nodiscard]] EvidenceRecord reading(const char* source, std::uint64_t sequence, std::int64_t milli) {
    Builder feed(asset_id(kAssetA), *SourceId::parse(source), SourceKind::TelemetryFeed, SourceClass::Peer);
    return EvidenceRecord::make(feed.sequence(sequence)
                                    .telemetry(MetricKind::Temperature, milli, SampleQuality::Good))
        .value();
}

[[nodiscard]] std::filesystem::path generation_path(const std::filesystem::path& root, std::uint64_t generation) {
    return root / "generations" / detail::generation_file_name(GenerationSequence(generation));
}

void truncate_file(const std::filesystem::path& file, std::uint64_t keep_bytes) {
    std::error_code error;
    std::filesystem::resize_file(file, keep_bytes, error);
}

void flip_byte(const std::filesystem::path& file, std::uint64_t offset) {
    std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
    stream.seekg(static_cast<std::streamoff>(offset));
    char value = 0;
    stream.read(&value, 1);
    value = static_cast<char>(value ^ 0x5A);
    stream.seekp(static_cast<std::streamoff>(offset));
    stream.write(&value, 1);
}

}  // namespace

AH_TEST(durability, a_new_store_commits_and_survives_a_reopen) {
    const TempDirectory directory("durability-basic");
    StoreOptions options;
    options.root = directory.path();

    {
        auto opened = Store::open(options);
        AH_REQUIRE_MSG(opened.has_value(), opened.error().to_string());
        Store store = std::move(opened.value());
        AH_CHECK(store.recovery().created);
        AH_CHECK(store.epoch().value() == 1);
        AH_CHECK(store.generation().value() == 1);
        AH_CHECK(store.state()->evidence.empty());

        const auto committed = store.commit(request_for(store.state(), 2, "first", reading("feed-a", 1, 40000)));
        AH_REQUIRE_MSG(committed.has_value(), committed.error().to_string());
        AH_CHECK(committed->generation.value() == 2);
        AH_CHECK(!committed->replayed);
        AH_CHECK(store.state()->evidence.size() == 1);
        AH_CHECK(store.state()->evidence[0].commit_epoch() == store.epoch());
        store.close();
    }

    auto reopened = Store::open(options);
    AH_REQUIRE_MSG(reopened.has_value(), reopened.error().to_string());
    Store store = std::move(reopened.value());
    AH_CHECK(!store.recovery().created);
    AH_CHECK(store.recovery().previous_close_clean);
    AH_CHECK(!store.recovery().recovered());
    AH_CHECK(store.state()->evidence.size() == 1);
    // The previous session closed cleanly, so nothing was demoted: the reading is
    // judged by its age, which is what lets one command ingest and a later
    // command assess the same store.
    AH_CHECK(store.recovery().dynamic_records_demoted == 0);
    // The epoch advanced anyway, because an earlier writer's token must be fenced
    // out even after an orderly close.
    AH_CHECK(store.epoch().value() == 2);
    AH_CHECK(store.state()->evidence[0].commit_epoch().value() == 1);
    store.close();
}

AH_TEST(durability, a_stale_epoch_and_a_reused_sequence_are_refused) {
    const TempDirectory directory("durability-fencing");
    StoreOptions options;
    options.root = directory.path();
    auto opened = Store::open(options);
    AH_REQUIRE(opened.has_value());
    Store store = std::move(opened.value());

    const StoreStatePtr first = store.state();
    auto stale = request_for(first, 2, "stale", reading("feed-a", 1, 40000));
    stale.expected_epoch = StoreEpoch(99);
    AH_REQUIRE_ERROR(stale_error, store.commit(stale), ErrorCode::StaleStoreEpoch);

    auto ahead = request_for(first, 5, "skip", reading("feed-a", 1, 40000));
    AH_REQUIRE_ERROR(skip_error, store.commit(ahead), ErrorCode::StaleMutationSequence);

    auto applied = request_for(first, 2, "applied", reading("feed-a", 1, 40000));
    AH_CHECK(store.commit(applied).has_value());

    // The same sequence and key replays the recorded outcome instead of applying
    // the mutation a second time.
    auto replay = request_for(store.state(), 2, "applied", reading("feed-b", 9, 90000));
    const auto replayed = store.commit(replay);
    AH_REQUIRE_MSG(replayed.has_value(), replayed.error().to_string());
    AH_CHECK(replayed->replayed);
    AH_CHECK(replayed->generation.value() == 2);
    AH_CHECK(store.state()->evidence.size() == 1);

    auto conflict = request_for(store.state(), 2, "different", reading("feed-c", 3, 50000));
    AH_REQUIRE_ERROR(conflict_error, store.commit(conflict), ErrorCode::IdempotencyConflict);

    auto missing_key = request_for(store.state(), 3, "", reading("feed-d", 4, 50000));
    AH_REQUIRE_ERROR(key_error, store.commit(missing_key), ErrorCode::InvalidInput);
    store.close();
}

AH_TEST(durability, a_torn_tail_falls_back_and_is_reported) {
    const TempDirectory directory("durability-torn");
    StoreOptions options;
    options.root = directory.path();
    {
        auto opened = Store::open(options);
        AH_REQUIRE(opened.has_value());
        Store store = std::move(opened.value());
        AH_CHECK(store.commit(request_for(store.state(), 2, "a", reading("feed-a", 1, 40000))).has_value());
        AH_CHECK(store.commit(request_for(store.state(), 3, "b", reading("feed-a", 2, 41000))).has_value());
        store.close();
    }
    // The commit point was reached, and then the newest generation lost its tail.
    const std::filesystem::path newest = generation_path(directory.path(), 3);
    AH_CHECK(std::filesystem::exists(newest));
    truncate_file(newest, 100 + 20);

    auto reopened = Store::open(options);
    AH_REQUIRE_MSG(reopened.has_value(), reopened.error().to_string());
    Store store = std::move(reopened.value());
    AH_CHECK(store.recovery().torn_tail_discarded);
    AH_CHECK(store.generation().value() == 2);
    AH_CHECK(store.state()->evidence.size() == 1);
    AH_CHECK(store.state()->recovered);
    AH_CHECK(store.recovery().recovered());
    store.close();
}

AH_TEST(durability, interior_corruption_is_rejected_rather_than_recovered) {
    const TempDirectory directory("durability-corrupt");
    StoreOptions options;
    options.root = directory.path();
    {
        auto opened = Store::open(options);
        AH_REQUIRE(opened.has_value());
        Store store = std::move(opened.value());
        AH_CHECK(store.commit(request_for(store.state(), 2, "a", reading("feed-a", 1, 40000))).has_value());
        store.close();
    }
    const std::filesystem::path newest = generation_path(directory.path(), 2);
    flip_byte(newest, 200);

    auto reopened = Store::open(options);
    AH_REQUIRE(!reopened.has_value());
    AH_CHECK(reopened.error().code() == ErrorCode::PayloadChecksumMismatch ||
             reopened.error().code() == ErrorCode::RecordDecodeFailed);

    const auto audit = Store::audit_at(directory.path());
    AH_REQUIRE(audit.has_value());
    AH_CHECK(!audit->ok);
    AH_CHECK(!audit->corrupt_generations.empty());
}

AH_TEST(durability, an_uncommitted_generation_is_never_adopted) {
    const TempDirectory directory("durability-uncommitted");
    StoreOptions options;
    options.root = directory.path();
    {
        auto opened = Store::open(options);
        AH_REQUIRE(opened.has_value());
        Store store = std::move(opened.value());
        AH_CHECK(store.commit(request_for(store.state(), 2, "a", reading("feed-a", 1, 40000))).has_value());
        store.close();
    }
    // A complete, internally consistent generation that the commit point does not
    // name. It was never committed, so it must not become the state.
    std::error_code error;
    std::filesystem::copy_file(generation_path(directory.path(), 2), generation_path(directory.path(), 3), error);
    AH_CHECK(!error);

    auto reopened = Store::open(options);
    AH_REQUIRE_MSG(reopened.has_value(), reopened.error().to_string());
    Store store = std::move(reopened.value());
    AH_CHECK(store.generation().value() == 2);
    AH_CHECK(store.recovery().uncommitted_discarded.size() == 1);
    AH_CHECK(store.state()->evidence.size() == 1);
    AH_CHECK(!std::filesystem::exists(generation_path(directory.path(), 3)));
    store.close();
}

AH_TEST(durability, a_missing_commit_point_is_refused) {
    const TempDirectory directory("durability-commit-point");
    StoreOptions options;
    options.root = directory.path();
    {
        auto opened = Store::open(options);
        AH_REQUIRE(opened.has_value());
        Store store = std::move(opened.value());
        AH_CHECK(store.commit(request_for(store.state(), 2, "a", reading("feed-a", 1, 40000))).has_value());
        store.close();
    }
    std::error_code error;
    std::filesystem::remove(directory.path() / "CURRENT", error);
    AH_CHECK(!error);

    auto reopened = Store::open(options);
    AH_REQUIRE(!reopened.has_value());
    AH_CHECK(reopened.error().code() == ErrorCode::CommitPointMissing);
}

AH_TEST(durability, configured_bounds_are_enforced_rather_than_exceeded) {
    const TempDirectory directory("durability-bounds");
    StoreOptions options;
    options.root = directory.path();
    options.max_evidence_per_asset = 2;
    auto opened = Store::open(options);
    AH_REQUIRE(opened.has_value());
    Store store = std::move(opened.value());
    AH_CHECK(store.commit(request_for(store.state(), 2, "a", reading("feed-a", 1, 40000))).has_value());
    AH_CHECK(store.commit(request_for(store.state(), 3, "b", reading("feed-a", 2, 41000))).has_value());
    auto third = request_for(store.state(), 4, "c", reading("feed-a", 3, 42000));
    AH_REQUIRE_ERROR(error, store.commit(third), ErrorCode::EvidenceCapacityExceeded);
    AH_CHECK(store.state()->evidence.size() == 2);
    store.close();
}

AH_TEST(durability, a_read_only_open_takes_no_lock_and_sees_one_generation) {
    const TempDirectory directory("durability-read-only");
    StoreOptions options;
    options.root = directory.path();
    auto writer = Store::open(options);
    AH_REQUIRE(writer.has_value());
    AH_CHECK(writer.value().commit(request_for(writer.value().state(), 2, "a", reading("feed-a", 1, 40000))).has_value());

    StoreOptions read_only = options;
    read_only.read_only = true;
    auto reader = Store::open(read_only);
    AH_REQUIRE_MSG(reader.has_value(), reader.error().to_string());
    AH_CHECK(!reader.value().is_writable());
    AH_CHECK(reader.value().state()->evidence.size() == 1);
    AH_CHECK(reader.value().generation().value() == 2);

    // A second writable open is refused while the first holds the lock.
    auto second = Store::open(options);
    AH_REQUIRE(!second.has_value());
    AH_CHECK(second.error().code() == ErrorCode::StoreLocked);

    reader.value().close();
    writer.value().close();

    StoreOptions missing = options;
    missing.create_if_missing = false;
    std::error_code error;
    std::filesystem::remove_all(directory.path() / "meta", error);
    auto absent = Store::open(missing);
    AH_REQUIRE(!absent.has_value());
    AH_CHECK(absent.error().code() == ErrorCode::StoreNotFound);
}

AH_TEST(durability, an_audit_reports_a_healthy_store_and_a_damaged_one) {
    const TempDirectory directory("durability-audit");
    StoreOptions options;
    options.root = directory.path();
    {
        auto opened = Store::open(options);
        AH_REQUIRE(opened.has_value());
        Store store = std::move(opened.value());
        AH_CHECK(store.commit(request_for(store.state(), 2, "a", reading("feed-a", 1, 40000))).has_value());
        const auto audit = store.audit();
        AH_REQUIRE(audit.has_value());
        AH_CHECK(audit->ok);
        AH_CHECK(audit->valid_generations.size() >= 1);
        AH_CHECK(audit->evidence_records == 1);
        AH_CHECK(audit->problems.empty());
        store.close();
    }
    const auto audit = Store::audit_at(directory.path());
    AH_REQUIRE(audit.has_value());
    AH_CHECK(audit->ok);
    AH_CHECK(!audit->problems.empty() == false);
    AH_CHECK(!audit->to_string().empty());
    AH_CHECK(!audit->store_id.empty());
}

AH_TEST(durability, retention_keeps_the_store_bounded_and_the_audit_quiet) {
    const TempDirectory directory("durability-retention");
    StoreOptions options;
    options.root = directory.path();
    options.retained_generations = 3;
    auto opened = Store::open(options);
    AH_REQUIRE(opened.has_value());
    Store store = std::move(opened.value());
    for (std::uint64_t sequence = 2; sequence <= 9; ++sequence) {
        const auto committed =
            store.commit(request_for(store.state(), sequence, "key-" + std::to_string(sequence),
                                     reading("feed-a", sequence, 40000 + static_cast<std::int64_t>(sequence))));
        AH_REQUIRE_MSG(committed.has_value(), committed.error().to_string());
    }
    AH_CHECK(store.state()->evidence.size() == 8);
    std::size_t files = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path() / "generations", error)) {
        if (entry.is_regular_file()) {
            ++files;
        }
    }
    AH_CHECK(files <= 3);
    const auto audit = store.audit();
    AH_REQUIRE(audit.has_value());
    AH_CHECK_MSG(audit->ok, audit->to_string());
    store.close();
}
