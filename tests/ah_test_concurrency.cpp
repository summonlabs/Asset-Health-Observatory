// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency: asynchronous ingest, concurrent readers during writes, ticket
// cancellation and shutdown. There is no timeout anywhere in this file: a case
// that does not finish is a defect, and it is reported by never returning.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "asset_health/export.hpp"
#include "asset_health/observatory.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

const Instant kBase = instant("2026-05-01T00:00:00Z");

[[nodiscard]] EvidenceRecord::Parts stream_parts(const char* source, std::uint64_t sequence, std::int64_t milli,
                                                 Instant observed) {
    Builder feed(asset_id(kAssetA), *SourceId::parse(source), SourceKind::TelemetryFeed, SourceClass::Peer);
    return feed.sequence(sequence).observed(observed).telemetry(MetricKind::Temperature, milli, SampleQuality::Good);
}

}  // namespace

AH_TEST(concurrency, asynchronous_ingest_admits_every_statement_exactly_once) {
    const TempDirectory directory("concurrency-ingest");
    ObservatoryOptions options;
    options.store_root = directory.path();
    options.ingest_workers = 4;
    options.ingest_queue_depth = 256;
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());

    constexpr std::uint64_t kStreams = 4;
    constexpr std::uint64_t kPerStream = 25;
    std::vector<IngestTicket> tickets;
    for (std::uint64_t stream = 0; stream < kStreams; ++stream) {
        const std::string source = "feed-" + std::to_string(stream);
        for (std::uint64_t sequence = 1; sequence <= kPerStream; ++sequence) {
            const auto observed = *kBase.shifted(Duration::from_seconds(static_cast<std::int64_t>(sequence)));
            auto ticket = observatory.submit(stream_parts(source.c_str(), sequence,
                                                         40000 + static_cast<std::int64_t>(sequence), observed));
            AH_REQUIRE_MSG(ticket.has_value(), ticket.error().to_string());
            tickets.push_back(ticket.value());
        }
    }
    AH_CHECK(observatory.pending() == tickets.size());
    AH_REQUIRE(observatory.drain().has_value());
    AH_CHECK(observatory.pending() == 0);

    std::size_t admitted = 0;
    for (const IngestTicket& ticket : tickets) {
        auto outcome = observatory.ticket(ticket);
        AH_REQUIRE(outcome.has_value());
        AH_CHECK(outcome.value().settled);
        AH_CHECK(outcome.value().failure.ok());
        AH_REQUIRE(outcome.value().result.admitted);
        ++admitted;
    }
    AH_CHECK(admitted == kStreams * kPerStream);

    auto records = observatory.evidence_for(asset_id(kAssetA));
    AH_REQUIRE(records.has_value());
    AH_CHECK(records.value().size() == kStreams * kPerStream);

    // The assessment is a function of the set, not of the order the workers
    // happened to commit in.
    auto first = observatory.assess(asset_id(kAssetA), *kBase.shifted(Duration::from_seconds(30)));
    auto second = observatory.assess(asset_id(kAssetA), *kBase.shifted(Duration::from_seconds(30)));
    AH_REQUIRE(first.has_value());
    AH_REQUIRE(second.has_value());
    AH_CHECK(first.value().explain() == second.value().explain());
    observatory.close();
}

AH_TEST(concurrency, readers_never_block_a_writer_and_never_see_a_torn_state) {
    const TempDirectory directory("concurrency-readers");
    ObservatoryOptions options;
    options.store_root = directory.path();
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());

    std::atomic<bool> writing{true};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> malformed{0};
    std::vector<std::thread> readers;
    for (int index = 0; index < 3; ++index) {
        readers.emplace_back([&observatory, &writing, &reads, &malformed] {
            while (writing.load()) {
                const StoreStatePtr state = observatory.snapshot();
                if (state == nullptr) {
                    ++malformed;
                    continue;
                }
                // A published snapshot is immutable and internally consistent:
                // the evidence list is in canonical order and every record names
                // the incarnation it was admitted for.
                for (std::size_t position = 1; position < state->evidence.size(); ++position) {
                    if (!EvidenceRecord::canonical_less(state->evidence[position - 1], state->evidence[position])) {
                        ++malformed;
                    }
                }
                ++reads;
            }
        });
    }

    for (std::uint64_t sequence = 1; sequence <= 120; ++sequence) {
        const auto observed = *kBase.shifted(Duration::from_seconds(static_cast<std::int64_t>(sequence)));
        auto outcome = observatory.ingest(stream_parts("feed-a", sequence, 40000, observed));
        AH_REQUIRE_MSG(outcome.has_value(), outcome.error().to_string());
        AH_REQUIRE(outcome.value().admitted);
    }
    writing.store(false);
    for (std::thread& reader : readers) {
        reader.join();
    }
    AH_CHECK(malformed.load() == 0);
    AH_CHECK(reads.load() > 0);

    auto records = observatory.evidence_for(asset_id(kAssetA));
    AH_REQUIRE(records.has_value());
    AH_CHECK(records.value().size() == 120);
    observatory.close();
}

AH_TEST(concurrency, a_cancelled_attempt_reports_what_actually_happened) {
    const TempDirectory directory("concurrency-cancel");
    ObservatoryOptions options;
    options.store_root = directory.path();
    // One worker and a deep queue: the first attempt is taken immediately and the
    // rest stay queued while the cancels are issued.
    options.ingest_workers = 1;
    options.ingest_queue_depth = 128;
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());

    std::vector<IngestTicket> tickets;
    for (std::uint64_t sequence = 1; sequence <= 40; ++sequence) {
        const auto observed = *kBase.shifted(Duration::from_seconds(static_cast<std::int64_t>(sequence)));
        auto ticket = observatory.submit(stream_parts("feed-a", sequence, 40000, observed));
        AH_REQUIRE(ticket.has_value());
        tickets.push_back(ticket.value());
    }
    for (std::size_t index = 20; index < tickets.size(); ++index) {
        const Status cancelled = observatory.cancel(tickets[index]);
        AH_CHECK(cancelled.has_value());
    }
    AH_REQUIRE(observatory.drain().has_value());

    std::size_t admitted = 0;
    std::size_t cancelled = 0;
    for (const IngestTicket& ticket : tickets) {
        auto outcome = observatory.ticket(ticket);
        AH_REQUIRE(outcome.has_value());
        if (outcome.value().cancelled) {
            ++cancelled;
            AH_CHECK(outcome.value().result.refusal_code == ErrorCode::Cancelled);
            continue;
        }
        AH_REQUIRE(outcome.value().result.admitted);
        ++admitted;
    }
    // Whether a cancel wins a race with the worker is not fixed, but the answer
    // must be one of the two and must match the store.
    AH_CHECK(admitted + cancelled == tickets.size());
    auto records = observatory.evidence_for(asset_id(kAssetA));
    AH_REQUIRE(records.has_value());
    AH_CHECK(records.value().size() == admitted);

    // Cancelling a settled attempt is refused rather than reported as an effect.
    const Status late = observatory.cancel(tickets.front());
    AH_REQUIRE(!late.has_value());
    AH_CHECK(late.error().code() == ErrorCode::AlreadyClosed);
    observatory.close();
}

AH_TEST(concurrency, closing_while_work_is_queued_settles_every_attempt) {
    const TempDirectory directory("concurrency-close");
    ObservatoryOptions options;
    options.store_root = directory.path();
    options.ingest_workers = 2;
    options.ingest_queue_depth = 256;
    auto opened = Observatory::open(options);
    AH_REQUIRE(opened.has_value());
    Observatory observatory = std::move(opened.value());
    std::vector<IngestTicket> tickets;
    for (std::uint64_t sequence = 1; sequence <= 60; ++sequence) {
        const auto observed = *kBase.shifted(Duration::from_seconds(static_cast<std::int64_t>(sequence)));
        auto ticket = observatory.submit(stream_parts("feed-a", sequence, 40000, observed));
        AH_REQUIRE(ticket.has_value());
        tickets.push_back(ticket.value());
    }
    // Close with work still queued. Every attempt must settle, because a worker
    // drains the queue before it returns.
    observatory.close();
    std::size_t settled = 0;
    for (const IngestTicket& ticket : tickets) {
        auto outcome = observatory.ticket(ticket);
        if (!outcome.has_value()) {
            continue;
        }
        AH_CHECK(outcome.value().settled || outcome.value().cancelled);
        ++settled;
    }
    AH_CHECK(settled == tickets.size());

    // The store is closed and says so, rather than failing in some other way.
    auto after = observatory.ingest(stream_parts("feed-a", 999, 40000, kBase));
    AH_REQUIRE(!after.has_value());
    AH_CHECK(after.error().code() == ErrorCode::StoreClosed);
}

AH_TEST(concurrency, many_observatories_open_and_close_without_leaking_a_lock) {
    const TempDirectory directory("concurrency-open-close");
    for (int index = 0; index < 12; ++index) {
        ObservatoryOptions options;
        options.store_root = directory.path() / ("store-" + std::to_string(index));
        auto opened = Observatory::open(options);
        AH_REQUIRE_MSG(opened.has_value(), opened.error().to_string());
        Observatory observatory = std::move(opened.value());
        auto outcome = observatory.ingest(stream_parts("feed-a", 1, 40000, kBase));
        AH_REQUIRE(outcome.has_value());
        observatory.close();
    }
    // The same store may be reopened as soon as the previous holder closed it.
    ObservatoryOptions options;
    options.store_root = directory.path() / "store-0";
    for (int index = 0; index < 5; ++index) {
        auto opened = Observatory::open(options);
        AH_REQUIRE_MSG(opened.has_value(), opened.error().to_string());
        opened.value().close();
    }
}
