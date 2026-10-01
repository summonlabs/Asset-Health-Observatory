// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/observatory.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "asset_health/limits.hpp"
#include "store_format.hpp"

namespace asset_health {
namespace {

/// The incarnation an assessment of this asset targets: the one the newest
/// identity statement names, or, when identity has never been observed, the
/// highest incarnation any statement names. Returns std::nullopt when the asset
/// appears in no statement at all.
[[nodiscard]] std::optional<AssetGeneration> target_generation_of(const StoreState& state,
                                                                 const AssetRefId& asset) {
    std::optional<AssetGeneration> from_identity;
    Instant newest_identity{};
    bool have_identity = false;
    std::optional<AssetGeneration> highest;
    for (const EvidenceRecord& record : state.evidence) {
        if (record.subject().asset() != asset) {
            continue;
        }
        if (!highest.has_value() || record.subject().generation() > *highest) {
            highest = record.subject().generation();
        }
        if (record.kind() != EvidenceKind::IdentityStatement) {
            continue;
        }
        if (!have_identity || record.observed_at() > newest_identity) {
            newest_identity = record.observed_at();
            from_identity = record.subject().generation();
            have_identity = true;
        }
    }
    if (from_identity.has_value()) {
        return from_identity;
    }
    if (highest.has_value()) {
        return highest;
    }
    // Nothing was admitted, but a refused statement may still name the asset. An
    // operator asking about an asset whose statements were all refused is better
    // served by an answer that says so than by being told the asset is unknown.
    std::optional<AssetGeneration> from_refusal;
    for (const RefusalRecord& refusal : state.refusals) {
        if (!refusal.asset.has_value() || *refusal.asset != asset || refusal.generation.is_zero()) {
            continue;
        }
        if (!from_refusal.has_value() || refusal.generation > *from_refusal) {
            from_refusal = refusal.generation;
        }
    }
    return from_refusal;
}

[[nodiscard]] std::string refusal_detail(const Error& error) {
    std::string text = error.message();
    if (error.context().has_value()) {
        text.append(" (");
        text.append(error.context()->first);
        text.push_back('=');
        text.append(error.context()->second);
        text.push_back(')');
    }
    return text;
}

[[nodiscard]] Instant system_now() {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return Instant(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

}  // namespace

struct Observatory::Impl {
    ObservatoryOptions options{};
    HealthPolicy policy = HealthPolicy::standard();
    Store store;
    std::filesystem::path root;

    /// Serialises the read-decide-commit sequence of an ingest. A stream check
    /// that is not atomic with the commit it authorises would let two deliveries
    /// of one stream position both pass.
    std::mutex ingest_mutex;

    struct Ticket {
        std::mutex mutex;
        std::condition_variable condition;
        bool settled = false;
        std::atomic<bool> cancel_requested{false};
        IngestTicketOutcome outcome;
    };

    struct Item {
        TicketId id{};
        EvidenceRecord::Parts parts;
        std::shared_ptr<Ticket> ticket;
    };

    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    std::deque<std::shared_ptr<Item>> queue;
    /// Sources with a statement currently being committed. Guarded by queue_mutex.
    std::set<SourceId> busy_sources;
    bool stopping = false;
    std::vector<std::thread> workers;

    std::atomic<std::uint64_t> next_ticket{1};
    std::atomic<std::size_t> pending{0};

    mutable std::mutex tickets_mutex;
    std::condition_variable tickets_condition;
    std::map<TicketId, std::shared_ptr<Ticket>> tickets;
    std::deque<TicketId> ticket_order;

    bool closed = false;

    /// Marks an attempt complete and wakes anyone waiting on it.
    static void settle(const std::shared_ptr<Ticket>& ticket, IngestTicketOutcome outcome) {
        {
            const std::lock_guard<std::mutex> guard(ticket->mutex);
            ticket->outcome = std::move(outcome);
            ticket->settled = true;
        }
        ticket->condition.notify_all();
    }

    /// Records a refusal durably, so a later assessment can report that the
    /// evidence it saw is not everything the producers sent. Refusals are
    /// committed at their own sequence: a refusal is a fact about this store.
    [[nodiscard]] Status record_refusal(const RefusalRecord& refusal) {
        const StoreStatePtr state = store.state();
        if (state == nullptr) {
            return Error(ErrorCode::StoreClosed, "the store has no published state");
        }
        Mutation mutation;
        mutation.append_refusals.push_back(refusal);
        CommitRequest request;
        request.expected_epoch = state->epoch;
        const auto sequence = state->generation.next();
        if (!sequence.has_value()) {
            return Error(ErrorCode::StaleMutationSequence, "the store sequence is exhausted");
        }
        request.sequence = *sequence;
        request.idempotency_key = "refusal:" + refusal.source.str() + ":" +
                                  refusal.detail.substr(0, std::min<std::size_t>(refusal.detail.size(), 64));
        request.mutation = std::move(mutation);
        const auto committed = store.commit(request);
        if (!committed) {
            return committed.error();
        }
        return ok();
    }

    /// Turns a refused statement into a durable refusal record and an outcome.
    [[nodiscard]] Outcome<IngestOutcome> refuse(const EvidenceRecord::Parts& parts, ErrorCode code,
                                                std::string detail) {
        RefusalRecord refusal;
        if (!parts.subject.asset().is_nil()) {
            refusal.asset = parts.subject.asset();
        }
        refusal.generation = parts.subject.generation();
        refusal.source = parts.provenance.source();
        refusal.source_kind = parts.provenance.kind();
        refusal.claimed_kind = parts.payload.kind;
        refusal.code = code;
        refusal.detail = std::move(detail);
        refusal.received_at = parts.provenance.received_at();
        const Status recorded = record_refusal(refusal);
        if (!recorded) {
            return recorded.error();
        }
        IngestOutcome outcome;
        outcome.admitted = false;
        outcome.refusal_code = code;
        outcome.detail = refusal.detail;
        const StoreStatePtr after = store.state();
        if (after != nullptr) {
            outcome.generation = after->generation;
        }
        return outcome;
    }

    /// The one ingest path, used by the calling thread and by every worker.
    [[nodiscard]] Outcome<IngestOutcome> ingest_one(const EvidenceRecord::Parts& parts) {
        const auto built = EvidenceRecord::make(parts);
        if (!built) {
            const Error& error = built.error();
            return refuse(parts, error.code(), refusal_detail(error));
        }
        const EvidenceRecord& record = built.value();
        const std::uint64_t digest = detail::evidence_digest(record);

        const std::lock_guard<std::mutex> guard(ingest_mutex);
        const StoreStatePtr state = store.state();
        if (state == nullptr) {
            return Error(ErrorCode::StoreClosed, "the store has no published state");
        }

        for (const EvidenceRecord& existing : state->evidence) {
            if (existing.id() != record.id()) {
                continue;
            }
            if (detail::evidence_digest(existing) == digest) {
                IngestOutcome outcome;
                outcome.admitted = true;
                outcome.duplicate_delivery = true;
                outcome.evidence_id = existing.id();
                outcome.generation = state->generation;
                outcome.detail = "this statement was already admitted under the same identifier";
                return outcome;
            }
            return Error(ErrorCode::DuplicateEvidenceId, "an evidence identifier was reused for different content")
                .with_context("evidence", record.id().to_string());
        }

        SourceStreamState stream;
        stream.source = record.source();
        stream.kind = record.source_kind();
        stream.epoch = record.provenance().epoch();
        stream.last_sequence = record.provenance().sequence();
        stream.last_digest = digest;
        stream.last_observed_at = record.observed_at();
        stream.last_received_at = record.received_at();
        stream.last_evidence_id = record.id();

        if (const SourceStreamState* existing = state->stream_state(record.source()); existing != nullptr) {
            if (record.provenance().epoch() < existing->epoch) {
                return refuse(parts, ErrorCode::StaleSourceEpoch,
                              "the statement claims stream epoch " + record.provenance().epoch().to_string() +
                                  " but this source has already advanced to epoch " + existing->epoch.to_string());
            }
            if (record.provenance().epoch() == existing->epoch) {
                if (record.provenance().sequence() < existing->last_sequence) {
                    return refuse(parts, ErrorCode::ReplayedSourceSequence,
                                  "the statement claims sequence " + record.provenance().sequence().to_string() +
                                      " but this stream has already delivered " +
                                      existing->last_sequence.to_string());
                }
                if (record.provenance().sequence() == existing->last_sequence) {
                    if (existing->last_digest == digest) {
                        IngestOutcome outcome;
                        outcome.admitted = true;
                        outcome.duplicate_delivery = true;
                        outcome.evidence_id = existing->last_evidence_id;
                        outcome.generation = state->generation;
                        outcome.detail = "this stream position was already admitted with identical content";
                        return outcome;
                    }
                    return refuse(parts, ErrorCode::ReplayedSourceSequence,
                                  "stream position " + record.provenance().epoch().to_string() + "/" +
                                      record.provenance().sequence().to_string() +
                                      " was already delivered with different content");
                }
            }
        }

        const Instant observed = record.observed_at();
        if (observed > record.received_at() &&
            observed.minus(record.received_at()) > policy.clock_skew_tolerance()) {
            return refuse(parts, ErrorCode::FutureDatedObservation,
                          "the statement is dated " + observed.minus(record.received_at()).to_seconds_string() +
                              " ahead of its receipt, beyond the " +
                              policy.clock_skew_tolerance().to_seconds_string() + " skew tolerance");
        }

        Mutation mutation;
        mutation.append_evidence.push_back(record);
        mutation.update_streams.push_back(stream);
        CommitRequest request;
        request.expected_epoch = state->epoch;
        const auto sequence = state->generation.next();
        if (!sequence.has_value()) {
            return Error(ErrorCode::StaleMutationSequence, "the store sequence is exhausted");
        }
        request.sequence = *sequence;
        request.idempotency_key = "ingest:" + record.source().str() + ":" +
                                  record.provenance().epoch().to_string() + ":" +
                                  record.provenance().sequence().to_string();
        request.mutation = std::move(mutation);
        const auto committed = store.commit(request);
        if (!committed) {
            return committed.error();
        }
        IngestOutcome outcome;
        outcome.admitted = true;
        outcome.evidence_id = record.id();
        outcome.generation = committed.value().generation;
        outcome.duplicate_delivery = committed.value().replayed;
        return outcome;
    }
};

Observatory::Observatory() noexcept = default;

Observatory::~Observatory() { close(); }

Observatory::Observatory(Observatory&& other) noexcept : impl_(std::move(other.impl_)) {}

Observatory& Observatory::operator=(Observatory&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Outcome<Observatory> Observatory::open(const ObservatoryOptions& options) {
    if (options.ingest_workers > limits::kMaxIngestWorkers) {
        return Error(ErrorCode::InvalidConfiguration, "the ingest worker count exceeds the configured bound");
    }
    if (options.ingest_queue_depth == 0) {
        return Error(ErrorCode::InvalidConfiguration, "an ingest queue needs room");
    }
    if (options.policy.id().empty()) {
        return Error(ErrorCode::InvalidConfiguration, "the observatory needs a valid health policy");
    }
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->policy = options.policy;
    impl->root = options.store_root;

    StoreOptions store_options;
    store_options.root = options.store_root;
    store_options.create_if_missing = options.create_if_missing;
    store_options.read_only = options.read_only;
    store_options.max_evidence_per_asset = options.max_evidence_per_asset;
    store_options.max_assets = options.max_assets;
    store_options.max_history_per_asset = options.max_history_per_asset;
    auto store = Store::open(store_options);
    if (!store) {
        return store.error();
    }
    impl->store = std::move(store.value());

    if (!options.read_only && options.ingest_workers != 0) {
        impl->stopping = false;
        for (std::size_t index = 0; index < options.ingest_workers; ++index) {
            impl->workers.emplace_back([pointer = impl.get()] {
                for (;;) {
                    std::shared_ptr<Impl::Item> item;
                    {
                        std::unique_lock<std::mutex> lock(pointer->queue_mutex);
                        pointer->queue_condition.wait(lock, [pointer] {
                            return pointer->stopping || !pointer->queue.empty();
                        });
                        if (pointer->queue.empty()) {
                            if (pointer->stopping) {
                                return;
                            }
                            continue;
                        }
                        // Take the oldest statement whose source has nothing in
                        // flight. A stream position is meaningful only in order,
                        // so statements from one source are committed in the
                        // order they were queued even with several workers, while
                        // different sources still proceed in parallel.
                        auto chosen = std::find_if(
                            pointer->queue.begin(), pointer->queue.end(),
                            [pointer](const std::shared_ptr<Impl::Item>& candidate) {
                                return pointer->busy_sources.find(candidate->parts.provenance.source()) ==
                                       pointer->busy_sources.end();
                            });
                        if (chosen == pointer->queue.end()) {
                            // Every queued statement belongs to a source that is
                            // already in flight. A finishing worker notifies this
                            // condition, so waiting here cannot be indefinite.
                            pointer->queue_condition.wait(lock);
                            continue;
                        }
                        item = *chosen;
                        pointer->queue.erase(chosen);
                        pointer->busy_sources.insert(item->parts.provenance.source());
                    }
                    IngestTicketOutcome outcome;
                    outcome.id = item->id;
                    if (item->ticket->cancel_requested.load()) {
                        outcome.cancelled = true;
                        outcome.settled = true;
                        outcome.result.admitted = false;
                        outcome.result.refusal_code = ErrorCode::Cancelled;
                        outcome.result.detail = "the attempt was cancelled before it was committed";
                    } else {
                        auto result = pointer->ingest_one(item->parts);
                        if (result) {
                            outcome.result = std::move(result.value());
                            outcome.settled = true;
                        } else {
                            outcome.failure = result.error();
                        }
                    }
                    {
                        const std::lock_guard<std::mutex> guard(pointer->queue_mutex);
                        pointer->busy_sources.erase(item->parts.provenance.source());
                    }
                    // The attempt is settled before the pending count falls, so a
                    // caller waiting for the queue to empty cannot be released
                    // while an answer is still being written.
                    Impl::settle(item->ticket, std::move(outcome));
                    pointer->pending.fetch_sub(1);
                    pointer->tickets_condition.notify_all();
                    pointer->queue_condition.notify_all();
                }
            });
        }
    }

    Observatory observatory;
    observatory.impl_ = std::move(impl);
    return observatory;
}

bool Observatory::is_open() const noexcept { return impl_ != nullptr && !impl_->closed && impl_->store.is_open(); }

bool Observatory::is_writable() const noexcept { return impl_ != nullptr && impl_->store.is_writable(); }

StoreEpoch Observatory::epoch() const noexcept { return impl_ == nullptr ? StoreEpoch{} : impl_->store.epoch(); }

GenerationSequence Observatory::generation() const noexcept {
    return impl_ == nullptr ? GenerationSequence{} : impl_->store.generation();
}

const RecoveryReport& Observatory::recovery() const noexcept {
    static const RecoveryReport empty;
    return impl_ == nullptr ? empty : impl_->store.recovery();
}

const HealthPolicy& Observatory::policy() const noexcept {
    static const HealthPolicy standard = HealthPolicy::standard();
    return impl_ == nullptr ? standard : impl_->policy;
}

const std::filesystem::path& Observatory::root() const noexcept {
    static const std::filesystem::path empty;
    return impl_ == nullptr ? empty : impl_->root;
}

Outcome<IngestOutcome> Observatory::ingest(const EvidenceRecord::Parts& parts) {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    if (!impl_->store.is_writable()) {
        return Error(ErrorCode::StoreReadOnly, "the observatory was opened read-only");
    }
    return impl_->ingest_one(parts);
}

Outcome<std::vector<IngestOutcome>> Observatory::ingest_batch(const std::vector<EvidenceRecord::Parts>& parts) {
    std::vector<IngestOutcome> outcomes;
    outcomes.reserve(parts.size());
    for (const EvidenceRecord::Parts& item : parts) {
        auto outcome = ingest(item);
        if (!outcome) {
            return outcome.error();
        }
        outcomes.push_back(std::move(outcome.value()));
    }
    return outcomes;
}

Outcome<AssetGeneration> Observatory::target_generation(const AssetRefId& asset) const {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    const StoreStatePtr state = impl_->store.state();
    if (state == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store has no published state");
    }
    const auto generation = target_generation_of(*state, asset);
    if (!generation.has_value()) {
        return Error(ErrorCode::UnknownAsset, "no admitted statement names this asset")
            .with_context("asset", asset.to_string());
    }
    return *generation;
}

Outcome<HealthAssessment> Observatory::assess_generation(const AssetRefId& asset, AssetGeneration generation,
                                                         Instant now) const {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    const StoreStatePtr state = impl_->store.state();
    if (state == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store has no published state");
    }
    EvaluationRequest request;
    request.asset = asset;
    request.generation = generation;
    request.evidence = state->evidence_for(asset);
    for (const RefusalRecord& refusal : state->refusals) {
        if (refusal.asset.has_value() && *refusal.asset == asset) {
            request.refusals.push_back(refusal);
        }
    }
    EvaluationContext context;
    context.now = now;
    context.store_epoch = state->epoch;
    context.live_source_epochs = state->newest_source_epochs();
    context.store_recovered = state->recovered;
    return evaluate_checked(request, impl_->policy, context);
}

Outcome<HealthAssessment> Observatory::assess(const AssetRefId& asset, Instant now) const {
    const auto generation = target_generation(asset);
    if (!generation) {
        return generation.error();
    }
    return assess_generation(asset, generation.value(), now);
}

Outcome<HealthAssessment> Observatory::assess_and_publish(const AssetRefId& asset, Instant now) {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    if (!impl_->store.is_writable()) {
        return Error(ErrorCode::StoreReadOnly, "the observatory was opened read-only");
    }
    auto assessment = assess(asset, now);
    if (!assessment) {
        return assessment.error();
    }
    HealthAssessment value = std::move(assessment.value());

    const std::lock_guard<std::mutex> guard(impl_->ingest_mutex);
    const StoreStatePtr state = impl_->store.state();
    if (state == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store has no published state");
    }
    AssessmentRevision revision{};
    for (const HistoryEntry& entry : state->history) {
        if (entry.asset == value.asset && entry.generation == value.generation && entry.revision > revision) {
            revision = entry.revision;
        }
    }
    const auto next_revision = revision.next();
    if (!next_revision.has_value()) {
        return Error(ErrorCode::HistoryCapacityExceeded, "the assessment revision counter is exhausted");
    }
    value.revision = *next_revision;

    Mutation mutation;
    mutation.publish_history.push_back(make_history_entry(value, system_now()));
    CommitRequest request;
    request.expected_epoch = state->epoch;
    const auto sequence = state->generation.next();
    if (!sequence.has_value()) {
        return Error(ErrorCode::StaleMutationSequence, "the store sequence is exhausted");
    }
    request.sequence = *sequence;
    request.idempotency_key = "publish:" + value.asset.to_compact_string() + ":g" + value.generation.to_string() +
                              ":s" + state->generation.to_string();
    request.mutation = std::move(mutation);
    const auto committed = impl_->store.commit(request);
    if (!committed) {
        return committed.error();
    }
    return value;
}

StoreStatePtr Observatory::snapshot() const noexcept {
    return impl_ == nullptr ? StoreStatePtr{} : impl_->store.state();
}

Outcome<HistoryView> Observatory::history(const AssetRefId& asset) const {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    const StoreStatePtr state = impl_->store.state();
    if (state == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store has no published state");
    }
    const auto generation = target_generation_of(*state, asset);
    if (!generation.has_value()) {
        return Error(ErrorCode::UnknownAsset, "no admitted statement names this asset")
            .with_context("asset", asset.to_string());
    }
    HistoryView view = build_history(state->history, asset, *generation);
    view.truncated = view.entries.size() >= impl_->options.max_history_per_asset;
    return view;
}

Outcome<std::vector<EvidenceRecord>> Observatory::evidence_for(const AssetRefId& asset) const {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    const StoreStatePtr state = impl_->store.state();
    if (state == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store has no published state");
    }
    return state->evidence_for(asset);
}

Outcome<std::vector<RefusalRecord>> Observatory::refusals_for(const AssetRefId& asset) const {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    const StoreStatePtr state = impl_->store.state();
    if (state == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store has no published state");
    }
    std::vector<RefusalRecord> refusals;
    for (const RefusalRecord& refusal : state->refusals) {
        if (refusal.asset.has_value() && *refusal.asset == asset) {
            refusals.push_back(refusal);
        }
    }
    return refusals;
}

Outcome<StoreAudit> Observatory::verify() const {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    return impl_->store.audit();
}

Outcome<IngestTicket> Observatory::submit(const EvidenceRecord::Parts& parts) {
    if (!is_open()) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    if (!impl_->store.is_writable()) {
        return Error(ErrorCode::StoreReadOnly, "the observatory was opened read-only");
    }
    if (impl_->workers.empty()) {
        return Error(ErrorCode::InvalidConfiguration,
                     "this observatory has no ingest workers, so every ingest is synchronous");
    }
    auto item = std::make_shared<Impl::Item>();
    item->id = TicketId(impl_->next_ticket.fetch_add(1));
    item->parts = parts;
    item->ticket = std::make_shared<Impl::Ticket>();
    // The pending count is raised before the item is queued, so a worker that
    // finishes instantly cannot decrement it below zero.
    impl_->pending.fetch_add(1);
    {
        const std::lock_guard<std::mutex> guard(impl_->queue_mutex);
        if (impl_->stopping) {
            impl_->pending.fetch_sub(1);
            return Error(ErrorCode::ShuttingDown, "the observatory is stopping");
        }
        if (impl_->queue.size() >= impl_->options.ingest_queue_depth) {
            impl_->pending.fetch_sub(1);
            return Error(ErrorCode::QueueFull, "the ingest queue is full")
                .with_context("depth", std::to_string(impl_->options.ingest_queue_depth));
        }
        impl_->queue.push_back(item);
    }
    {
        const std::lock_guard<std::mutex> guard(impl_->tickets_mutex);
        impl_->tickets[item->id] = item->ticket;
        impl_->ticket_order.push_back(item->id);
        while (impl_->ticket_order.size() > limits::kMaxRetainedTickets) {
            impl_->tickets.erase(impl_->ticket_order.front());
            impl_->ticket_order.pop_front();
        }
    }
    impl_->queue_condition.notify_one();
    IngestTicket handle;
    handle.id = item->id;
    handle.valid = true;
    return handle;
}

Outcome<IngestTicketOutcome> Observatory::ticket(IngestTicket handle) const {
    if (impl_ == nullptr || !handle.valid) {
        return Error(ErrorCode::TicketUnknown, "the ticket handle is not valid");
    }
    std::shared_ptr<Impl::Ticket> found;
    {
        const std::lock_guard<std::mutex> guard(impl_->tickets_mutex);
        const auto entry = impl_->tickets.find(handle.id);
        if (entry == impl_->tickets.end()) {
            return Error(ErrorCode::TicketUnknown, "no attempt is retained under this ticket")
                .with_context("ticket", handle.id.to_string());
        }
        found = entry->second;
    }
    const std::lock_guard<std::mutex> guard(found->mutex);
    IngestTicketOutcome outcome = found->outcome;
    outcome.id = handle.id;
    return outcome;
}

Status Observatory::wait(IngestTicket handle) {
    if (impl_ == nullptr || !handle.valid) {
        return Error(ErrorCode::TicketUnknown, "the ticket handle is not valid");
    }
    std::shared_ptr<Impl::Ticket> found;
    {
        const std::lock_guard<std::mutex> guard(impl_->tickets_mutex);
        const auto entry = impl_->tickets.find(handle.id);
        if (entry == impl_->tickets.end()) {
            return Error(ErrorCode::TicketUnknown, "no attempt is retained under this ticket")
                .with_context("ticket", handle.id.to_string());
        }
        found = entry->second;
    }
    // No timeout: a wait that never returns is a defect to diagnose, not a case
    // to abandon. Every queued attempt settles, because a worker either commits
    // it, cancels it, or records why it could not commit it.
    std::unique_lock<std::mutex> lock(found->mutex);
    found->condition.wait(lock, [found] { return found->settled; });
    return ok();
}

Status Observatory::cancel(IngestTicket handle) {
    if (impl_ == nullptr || !handle.valid) {
        return Error(ErrorCode::TicketUnknown, "the ticket handle is not valid");
    }
    std::shared_ptr<Impl::Ticket> found;
    {
        const std::lock_guard<std::mutex> guard(impl_->tickets_mutex);
        const auto entry = impl_->tickets.find(handle.id);
        if (entry == impl_->tickets.end()) {
            return Error(ErrorCode::TicketUnknown, "no attempt is retained under this ticket")
                .with_context("ticket", handle.id.to_string());
        }
        found = entry->second;
    }
    const std::lock_guard<std::mutex> guard(found->mutex);
    if (found->settled) {
        // An attempt that has already settled cannot be cancelled. Saying so is
        // the honest answer; reporting success would claim an effect that did not
        // happen.
        return Error(ErrorCode::AlreadyClosed, "the attempt has already settled and cannot be cancelled");
    }
    found->cancel_requested.store(true);
    return ok();
}

Status Observatory::drain() {
    if (impl_ == nullptr) {
        return Error(ErrorCode::StoreClosed, "the observatory is not open");
    }
    std::unique_lock<std::mutex> lock(impl_->tickets_mutex);
    impl_->tickets_condition.wait(lock, [this] { return impl_->pending.load() == 0; });
    return ok();
}

std::size_t Observatory::pending() const { return impl_ == nullptr ? 0 : impl_->pending.load(); }

void Observatory::close() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->closed) {
        return;
    }
    if (!impl_->workers.empty()) {
        {
            const std::lock_guard<std::mutex> guard(impl_->queue_mutex);
            impl_->stopping = true;
        }
        impl_->queue_condition.notify_all();
        // The workers are joined without holding any lock they need: no mutex is
        // held across this join, so a worker that is mid-ingest can finish and
        // release the ingest mutex.
        for (std::thread& worker : impl_->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        impl_->workers.clear();
    }
    impl_->closed = true;
    impl_->store.close();
    impl_->tickets_condition.notify_all();
}

}  // namespace asset_health
