// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The observatory runtime: admit evidence, answer health questions, keep the
// answer durable.
//
// Three promises shape this interface.
//
//  1. A refusal is an answer. A statement that fails admission returns a
//     successful outcome that says "not admitted, and here is why", and the
//     refusal is durable, so a later assessment can report that the evidence it
//     saw is not everything the producers sent.
//  2. An assessment is a function of the admitted evidence. Two observatories
//     given the same evidence, policy and instant produce the same answer, in
//     the same order, with the same arithmetic.
//  3. Reading never blocks writing. An assessment works on an immutable
//     snapshot; a reader holds it for as long as it likes.

#ifndef ASSET_HEALTH_OBSERVATORY_HPP
#define ASSET_HEALTH_OBSERVATORY_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asset_health/assessment.hpp"
#include "asset_health/error.hpp"
#include "asset_health/evidence.hpp"
#include "asset_health/evaluation.hpp"
#include "asset_health/history.hpp"
#include "asset_health/limits.hpp"
#include "asset_health/persistence.hpp"
#include "asset_health/policy.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// The result of offering one statement: admitted, or refused with a reason.
struct IngestOutcome {
    bool admitted = false;
    /// The identifier of the admitted record; nil when the statement was refused.
    EvidenceId evidence_id;
    /// The generation the outcome was committed in.
    GenerationSequence generation{};
    /// Why the statement was refused. ErrorCode::None when it was admitted.
    ErrorCode refusal_code = ErrorCode::None;
    /// One line naming the specific problem, for the operator.
    std::string detail;
    /// True when this delivery repeated a statement already admitted from the
    /// same stream position, so nothing new was written.
    bool duplicate_delivery = false;
};

/// Identity of an asynchronous ingest attempt.
struct TicketIdTag;
using TicketId = detail::Counter<TicketIdTag, std::uint64_t>;

/// A handle on an asynchronous ingest attempt.
struct IngestTicket {
    TicketId id{};
    bool valid = false;
};

/// The settled state of an asynchronous ingest attempt.
struct IngestTicketOutcome {
    TicketId id{};
    bool settled = false;
    bool cancelled = false;
    IngestOutcome result;
    /// Set when the attempt failed for an infrastructure reason rather than
    /// being refused as evidence.
    Error failure;
};

struct ObservatoryOptions {
    std::filesystem::path store_root;
    /// The policy every assessment is made under. Copied into the observatory.
    HealthPolicy policy = HealthPolicy::standard();
    bool create_if_missing = true;
    bool read_only = false;
    /// Ingest workers. Zero runs ingest on the calling thread, which is the
    /// default: an operator tool wants a refusal before the next command.
    std::size_t ingest_workers = 0;
    /// Depth of the asynchronous ingest queue.
    std::size_t ingest_queue_depth = 1024;
    /// Bounds forwarded to the store.
    std::size_t max_evidence_per_asset = limits::kMaxEvidencePerAsset;
    std::size_t max_assets = limits::kMaxAssets;
    std::size_t max_history_per_asset = limits::kMaxHistoryPerAsset;
    /// Overrides for the freshness clock. When absent, a manual clock starting at
    /// the supplied instant is used, and an ingest stamps received_at from it.
    std::optional<Instant> clock_override;
};

/// The runtime.
class ASSET_HEALTH_API Observatory {
public:
    Observatory() noexcept;
    ~Observatory();
    Observatory(Observatory&& other) noexcept;
    Observatory& operator=(Observatory&& other) noexcept;
    Observatory(const Observatory&) = delete;
    Observatory& operator=(const Observatory&) = delete;

    [[nodiscard]] static Outcome<Observatory> open(const ObservatoryOptions& options);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_writable() const noexcept;
    [[nodiscard]] StoreEpoch epoch() const noexcept;
    [[nodiscard]] GenerationSequence generation() const noexcept;
    [[nodiscard]] const RecoveryReport& recovery() const noexcept;
    [[nodiscard]] const HealthPolicy& policy() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    /// Admits one statement and commits it. Admission validates provenance,
    /// authority domain, payload consistency, stream position and bounds, and
    /// refuses rather than repairs.
    [[nodiscard]] Outcome<IngestOutcome> ingest(const EvidenceRecord::Parts& parts);

    /// Admits a batch as one commit. The batch is validated statement by
    /// statement in the order given; the first refusal is returned and the
    /// statements before it stay admitted, because they were already valid and
    /// dropping them would lose evidence.
    [[nodiscard]] Outcome<std::vector<IngestOutcome>> ingest_batch(
        const std::vector<EvidenceRecord::Parts>& parts);

    /// Evaluates an asset at an instant without publishing the answer.
    [[nodiscard]] Outcome<HealthAssessment> assess(const AssetRefId& asset, Instant now) const;

    /// Evaluates and publishes the answer into durable history, returning it with
    /// its revision filled in.
    [[nodiscard]] Outcome<HealthAssessment> assess_and_publish(const AssetRefId& asset, Instant now);

    /// Evaluates for an explicitly named incarnation.
    [[nodiscard]] Outcome<HealthAssessment> assess_generation(const AssetRefId& asset,
                                                             AssetGeneration generation, Instant now) const;

    /// The immutable state the observatory has published. A reader may hold it
    /// for as long as it likes: it is replaced, never mutated, so holding it
    /// neither blocks a writer nor observes a partial write.
    [[nodiscard]] StoreStatePtr snapshot() const noexcept;

    [[nodiscard]] Outcome<HistoryView> history(const AssetRefId& asset) const;
    [[nodiscard]] Outcome<std::vector<EvidenceRecord>> evidence_for(const AssetRefId& asset) const;
    [[nodiscard]] Outcome<std::vector<RefusalRecord>> refusals_for(const AssetRefId& asset) const;
    [[nodiscard]] Outcome<StoreAudit> verify() const;

    /// The generation an assessment of this asset would target: the incarnation
    /// named by the newest identity statement, or, when no identity statement has
    /// been observed, the highest generation any statement names.
    [[nodiscard]] Outcome<AssetGeneration> target_generation(const AssetRefId& asset) const;

    // --- Asynchronous ingest ------------------------------------------------

    /// Queues one statement for a worker. Refused when no worker is configured
    /// or the queue is full.
    [[nodiscard]] Outcome<IngestTicket> submit(const EvidenceRecord::Parts& parts);

    /// The settled state of a ticket. A ticket that has not settled reports
    /// settled == false; an unknown ticket is a rejection.
    [[nodiscard]] Outcome<IngestTicketOutcome> ticket(IngestTicket handle) const;

    /// Waits until \p handle settles. Has no timeout: a wait that never returns
    /// is a defect to diagnose, not a case to abandon.
    [[nodiscard]] Status wait(IngestTicket handle);

    /// Asks for a queued attempt to be abandoned. An attempt already committed
    /// cannot be cancelled and reports its result.
    [[nodiscard]] Status cancel(IngestTicket handle);

    /// Waits until every queued attempt has settled.
    [[nodiscard]] Status drain();

    /// Number of attempts that have not settled.
    [[nodiscard]] std::size_t pending() const;

    /// Closes the store and stops the workers. Idempotent. Workers are stopped
    /// before the store is closed, and no worker is ever asked to take a lock the
    /// closing thread holds.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace asset_health

#endif  // ASSET_HEALTH_OBSERVATORY_HPP
