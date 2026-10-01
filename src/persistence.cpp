// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "asset_health/limits.hpp"
#include "crc32.hpp"
#include "fs_atomic.hpp"
#include "store_format.hpp"

namespace asset_health {
namespace {

constexpr const char* kMetaName = "meta";
constexpr const char* kLockName = "LOCK";
constexpr const char* kCurrentName = "CURRENT";
constexpr const char* kCleanName = "CLEAN";
constexpr const char* kGenerationsDir = "generations";

}  // namespace

std::map<SourceId, StreamEpoch> StoreState::newest_source_epochs() const {
    std::map<SourceId, StreamEpoch> epochs;
    for (const SourceStreamState& stream : source_streams) {
        const auto existing = epochs.find(stream.source);
        if (existing == epochs.end() || existing->second < stream.epoch) {
            epochs[stream.source] = stream.epoch;
        }
    }
    return epochs;
}

std::vector<EvidenceRecord> StoreState::evidence_for(const AssetRefId& asset) const {
    std::vector<EvidenceRecord> records;
    for (const EvidenceRecord& record : evidence) {
        if (record.subject().asset() == asset) {
            records.push_back(record);
        }
    }
    return records;
}

const SourceStreamState* StoreState::stream_state(const SourceId& source) const {
    for (const SourceStreamState& stream : source_streams) {
        if (stream.source == source) {
            return &stream;
        }
    }
    return nullptr;
}

std::string RecoveryReport::to_string() const {
    std::string text;
    text.append("created=");
    text.append(created ? "true" : "false");
    text.append(" existing=");
    text.append(existing ? "true" : "false");
    text.append(" previous-close-clean=");
    text.append(previous_close_clean ? "true" : "false");
    text.append(" torn-tail-discarded=");
    text.append(torn_tail_discarded ? "true" : "false");
    text.append(" previous-epoch=");
    text.append(previous_epoch.to_string());
    text.append(" current-epoch=");
    text.append(current_epoch.to_string());
    text.append(" generation=");
    text.append(recovered_generation.to_string());
    text.append(" evidence=");
    text.append(std::to_string(evidence_records));
    text.append(" history=");
    text.append(std::to_string(history_records));
    text.append(" dynamic-demoted=");
    text.append(std::to_string(dynamic_records_demoted));
    for (const GenerationSequence& generation : uncommitted_discarded) {
        text.append(" uncommitted=");
        text.append(generation.to_string());
    }
    for (const GenerationSequence& generation : damaged_below_recovered) {
        text.append(" damaged=");
        text.append(generation.to_string());
    }
    if (!detail.empty()) {
        text.append(" detail=");
        text.append(detail);
    }
    return text;
}

std::string StoreAudit::to_string() const {
    std::string text;
    text.append("store ");
    text.append(found ? store_id : std::string("(not found)"));
    text.append(" format-version ");
    text.append(format_version.to_string());
    text.append(" commit-point-generation ");
    text.append(current_generation.to_string());
    text.append(" epoch ");
    text.append(epoch.to_string());
    text.append(" generations ");
    text.append(std::to_string(generations.size()));
    text.append(" valid ");
    text.append(std::to_string(valid_generations.size()));
    text.append(" torn ");
    text.append(std::to_string(torn_generations.size()));
    text.append(" corrupt ");
    text.append(std::to_string(corrupt_generations.size()));
    text.append(" uncommitted ");
    text.append(std::to_string(uncommitted_generations.size()));
    text.append(" missing ");
    text.append(std::to_string(missing_generations.size()));
    text.append(" evidence ");
    text.append(std::to_string(evidence_records));
    text.append(" history ");
    text.append(std::to_string(history_records));
    text.append(" ok ");
    text.append(ok ? "true" : "false");
    for (const std::string& problem : problems) {
        text.append("\nproblem ");
        text.append(problem);
    }
    return text;
}

struct Store::Impl {
    StoreOptions options{};
    std::filesystem::path root;
    std::filesystem::path generations;
    std::atomic<StoreStatePtr> published{StoreStatePtr{}};
    RecoveryReport report;
    detail::fs::ExclusiveLock lock;
    bool writable = false;
    bool open = false;
    bool previous_close_clean = false;
    std::uint64_t current_payload_digest = 0;
    mutable std::mutex commit_mutex;
};

namespace {

[[nodiscard]] std::filesystem::path generations_directory(const std::filesystem::path& root) {
    return root / kGenerationsDir;
}

[[nodiscard]] std::vector<GenerationSequence> list_generations(const std::filesystem::path& directory,
                                                               bool* listed_ok = nullptr) {
    std::vector<GenerationSequence> found;
    const auto names = detail::fs::list_files(directory);
    if (!names) {
        if (listed_ok != nullptr) {
            *listed_ok = false;
        }
        return found;
    }
    if (listed_ok != nullptr) {
        *listed_ok = true;
    }
    for (const std::string& name : names.value()) {
        const auto generation = detail::parse_generation_file_name(name);
        if (generation.has_value()) {
            found.push_back(*generation);
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

[[nodiscard]] bool is_dynamic(EvidenceKind kind) noexcept {
    return freshness_class_of(kind) == FreshnessClass::Dynamic;
}

/// The revision a published history entry receives. The store assigns it, so a
/// caller cannot publish two different answers under one revision.
[[nodiscard]] AssessmentRevision next_history_revision(const StoreState& state, const AssetRefId& asset,
                                                       AssetGeneration generation,
                                                       AssessmentRevision requested) {
    AssessmentRevision highest{};
    for (const HistoryEntry& entry : state.history) {
        if (entry.asset == asset && entry.generation == generation && entry.revision > highest) {
            highest = entry.revision;
        }
    }
    if (!requested.is_zero() && requested > highest) {
        return requested;
    }
    const auto next = highest.next();
    return next.has_value() ? *next : AssessmentRevision{};
}

/// Loads one generation file and decodes it. Returns the specific error so the
/// caller can distinguish a torn tail from interior corruption.
[[nodiscard]] Status load_generation_file(const std::filesystem::path& file, StoreState& out,
                                          detail::GenerationHeader& header) {
    std::vector<std::uint8_t> bytes;
    const auto read = detail::fs::read_file(file, limits::kMaxGenerationPayloadBytes + 1024, bytes);
    if (!read) {
        return read.error();
    }
    if (!read.value()) {
        return Error(ErrorCode::StoreNotFound, "the generation file named by the commit point is absent")
            .with_context("path", file.filename().string());
    }
    return detail::decode_generation(bytes, out, header);
}

}  // namespace

Outcome<GenerationSequence> inspect_generation_file(const std::filesystem::path& file, std::uint64_t* payload_bytes) {
    std::vector<std::uint8_t> bytes;
    const auto read = detail::fs::read_file(file, limits::kMaxGenerationPayloadBytes + 1024, bytes);
    if (!read) {
        return read.error();
    }
    if (!read.value()) {
        return Error(ErrorCode::StoreNotFound, "the generation file does not exist")
            .with_context("path", file.filename().string());
    }
    detail::GenerationHeader header;
    StoreState state;
    const Status status = detail::decode_generation(bytes, state, header);
    if (!status) {
        return status.error();
    }
    if (payload_bytes != nullptr) {
        *payload_bytes = header.payload_length;
    }
    return GenerationSequence(header.generation);
}

Outcome<Store> Store::open(const StoreOptions& options) {
    if (options.root.empty()) {
        return Error(ErrorCode::InvalidConfiguration, "a store needs a directory");
    }
    if (options.retained_generations == 0) {
        return Error(ErrorCode::InvalidConfiguration, "at least one generation must be retained");
    }
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->root = options.root;
    impl->generations = generations_directory(options.root);
    impl->writable = !options.read_only;

    const auto meta_path = impl->root / kMetaName;
    const auto meta_exists = detail::fs::file_exists(meta_path);
    if (!meta_exists) {
        return meta_exists.error();
    }
    const auto current_path = impl->root / kCurrentName;
    const auto clean_path = impl->root / kCleanName;

    bool fresh = false;
    if (!meta_exists.value()) {
        if (options.read_only) {
            return Error(ErrorCode::StoreNotFound, "the store does not exist and this open is read-only")
                .with_context("path", options.root.string());
        }
        if (!options.create_if_missing) {
            return Error(ErrorCode::StoreNotFound, "the store does not exist and creating it was not requested")
                .with_context("path", options.root.string());
        }
        fresh = true;
    }

    const Status directory_status = detail::fs::ensure_directory(impl->root);
    if (!directory_status) {
        return directory_status.error();
    }

    if (impl->writable) {
        // The write lock is taken before anything is read, so a second writer
        // fails immediately rather than recovering a state it will not own.
        auto lock = detail::fs::ExclusiveLock::acquire(impl->root / kLockName, true);
        if (!lock) {
            return lock.error();
        }
        impl->lock = std::move(lock.value());
    }

    StoreState state;
    detail::MetaRecord meta;
    detail::CurrentRecord current;
    std::vector<std::uint8_t> bytes;

    if (fresh) {
        detail::UuidBytes store_id{};
        if (!generate_uuid_v4(store_id)) {
            return Error(ErrorCode::AllocationFailed, "the platform entropy source is unavailable, so no store "
                                                      "identifier could be generated");
        }
        meta.format_version = limits::kStoreFormatVersion;
        meta.store_id = store_id;
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        meta.created_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
        const Status meta_status = detail::fs::write_atomic(meta_path, detail::encode_meta(meta));
        if (!meta_status) {
            return meta_status.error();
        }
        const Status directory = detail::fs::ensure_directory(impl->generations);
        if (!directory) {
            return directory.error();
        }
        state.store_id = store_id;
        state.generation = GenerationSequence(1);
        state.epoch = StoreEpoch(1);
        detail::GenerationHeader header;
        header.format_version = limits::kStoreFormatVersion;
        header.generation = 1;
        header.epoch = 1;
        header.store_id = store_id;
        header.recorded_at_ns = meta.created_at_ns;
        const auto file_bytes = detail::encode_generation(state, header);
        const Status written = detail::fs::write_new(impl->generations / detail::generation_file_name(state.generation),
                                                     file_bytes);
        if (!written) {
            return written.error();
        }
        detail::CurrentRecord created;
        created.format_version = limits::kStoreFormatVersion;
        created.store_id = store_id;
        created.generation = state.generation.value();
        created.epoch = state.epoch.value();
        created.payload_digest = detail::payload_digest(detail::encode_payload(state, header));
        const Status commit_point = detail::fs::write_atomic(current_path, detail::encode_current(created));
        if (!commit_point) {
            return commit_point.error();
        }
        impl->current_payload_digest = created.payload_digest;
        impl->previous_close_clean = true;
        impl->report.created = true;
        impl->report.previous_epoch = StoreEpoch{};
        impl->report.current_epoch = state.epoch;
        impl->report.recovered_generation = state.generation;
    } else {
        const auto meta_read = detail::fs::read_file(meta_path, 4096, bytes);
        if (!meta_read) {
            return meta_read.error();
        }
        const Status meta_status = detail::decode_meta(bytes, meta);
        if (!meta_status) {
            return meta_status.error();
        }
        const auto clean_exists = detail::fs::file_exists(clean_path);
        if (!clean_exists) {
            return clean_exists.error();
        }
        impl->previous_close_clean = clean_exists.value();
        impl->report.previous_close_clean = impl->previous_close_clean;

        const auto current_read = detail::fs::read_file(current_path, 4096, bytes);
        if (!current_read) {
            return current_read.error();
        }
        if (!current_read.value()) {
            // Refusing here is the conservative choice. Without the commit point
            // there is no way to tell a committed generation from the debris of
            // an interrupted one, and adopting debris would promote uncommitted
            // dynamic evidence to current evidence.
            return Error(ErrorCode::CommitPointMissing,
                         "the store has no commit point, so no generation can be shown to have been committed");
        }
        const Status current_status = detail::decode_current(bytes, current);
        if (!current_status) {
            return current_status.error().with_context("path", current_path.string());
        }
        if (current.store_id != meta.store_id) {
            return Error(ErrorCode::StoreIntegrityFailed,
                         "the commit point and the store meta name different stores");
        }

        std::vector<GenerationSequence> available = list_generations(impl->generations);
        std::vector<GenerationSequence> above;
        for (const GenerationSequence& generation : available) {
            if (generation > GenerationSequence(current.generation)) {
                above.push_back(generation);
            }
        }
        const auto target = std::find(available.begin(), available.end(), GenerationSequence(current.generation));
        detail::GenerationHeader header;
        std::vector<GenerationSequence> damaged;
        bool recovered = false;
        if (target != available.end()) {
            const Status loaded = load_generation_file(impl->generations / detail::generation_file_name(*target), state,
                                                       header);
            if (loaded) {
                // The commit point names the generation and its digest. Both must
                // agree with the file, which catches a file replaced by a
                // different but internally consistent generation.
                const std::uint64_t actual = detail::payload_digest(detail::encode_payload(state, header));
                if (actual != current.payload_digest) {
                    return Error(ErrorCode::StoreIntegrityFailed,
                                 "the commit point digest does not match the generation it names")
                        .with_context("generation", std::to_string(current.generation));
                }
                impl->current_payload_digest = current.payload_digest;
            } else if (loaded.error().code() == ErrorCode::TornTailDiscarded) {
                // An interrupted write left the newest committed generation short.
                // The previous committed generation is used, and the loss is
                // reported rather than hidden.
                impl->report.torn_tail_discarded = true;
                impl->report.detail = "the newest committed generation was discarded because it is torn: " +
                                      loaded.error().message();
                recovered = true;
            } else {
                return std::move(loaded.error()).with_context("generation", std::to_string(current.generation));
            }
        } else {
            impl->report.detail = "the generation named by the commit point is absent from the store directory";
            recovered = true;
        }

        if (recovered) {
            bool found = false;
            for (auto iterator = available.rbegin(); iterator != available.rend(); ++iterator) {
                if (*iterator >= GenerationSequence(current.generation)) {
                    continue;
                }
                detail::GenerationHeader candidate_header;
                StoreState candidate;
                const Status loaded =
                    load_generation_file(impl->generations / detail::generation_file_name(*iterator), candidate,
                                         candidate_header);
                if (loaded) {
                    state = std::move(candidate);
                    header = candidate_header;
                    // The recovered generation becomes the commit point, so its
                    // own payload digest is what a later commit must chain from.
                    impl->current_payload_digest = detail::payload_digest(detail::encode_payload(state, header));
                    found = true;
                    break;
                }
                if (loaded.error().code() != ErrorCode::TornTailDiscarded) {
                    damaged.push_back(*iterator);
                    continue;
                }
                damaged.push_back(*iterator);
            }
            if (!found) {
                return Error(ErrorCode::StoreRecoveryImpossible,
                             "no fully valid generation was found below the one the commit point names");
            }
            impl->report.damaged_below_recovered = damaged;
        }

        state.recovered = impl->report.recovered();
        impl->report.existing = true;
        impl->report.previous_epoch = StoreEpoch(current.epoch);
        impl->report.uncommitted_discarded = above;
        impl->report.recovered_generation = state.generation;
        // The epoch a generation was written under is recorded in its header for
        // the chain audit, but the current epoch is the one the commit point
        // names: the header does not change when an epoch is published, so using
        // it here would let the epoch stop advancing.
        state.epoch = StoreEpoch(current.epoch);
    }

    // Only a recovered state demotes its dynamic evidence: an orderly restart
    // leaves a reading to be judged by its age, and a session that died leaves
    // nothing confirmed since it stopped.
    std::uint64_t demoted = 0;
    if (impl->report.recovered()) {
        for (const EvidenceRecord& record : state.evidence) {
            if (is_dynamic(record.kind())) {
                ++demoted;
            }
        }
    }
    impl->report.evidence_records = state.evidence.size();
    impl->report.history_records = state.history.size();
    impl->report.dynamic_records_demoted = demoted;

    if (impl->writable) {
        // A writable open publishes a strictly greater epoch and marks the store
        // dirty: from this instant an earlier writer's token is fenced out and a
        // crash is distinguishable from a clean close.
        const Status removed = detail::fs::remove_file(clean_path);
        if (!removed) {
            return removed.error();
        }
        const StoreEpoch next_epoch(state.epoch.value() + 1);
        // A fresh store already published epoch 1 with its first generation, and
        // that publication is the creation; an existing store advances here.
        const StoreEpoch published_epoch = impl->report.created ? state.epoch : next_epoch;
        state.epoch = published_epoch;
        impl->report.current_epoch = published_epoch;

        detail::CurrentRecord updated;
        updated.format_version = limits::kStoreFormatVersion;
        updated.store_id = state.store_id;
        updated.generation = state.generation.value();
        updated.epoch = published_epoch.value();
        updated.payload_digest = impl->current_payload_digest;
        const Status commit_point = detail::fs::write_atomic(impl->root / kCurrentName, detail::encode_current(updated));
        if (!commit_point) {
            return commit_point.error();
        }

        // Generations above the commit point were never committed. They are
        // removed so that a later commit cannot collide with them, and each one
        // is named in the recovery report.
        for (const GenerationSequence& generation : impl->report.uncommitted_discarded) {
            const Status removed_generation =
                detail::fs::remove_file(impl->generations / detail::generation_file_name(generation));
            if (!removed_generation) {
                return removed_generation.error();
            }
        }
        const auto leftovers = detail::fs::list_files(impl->generations);
        if (leftovers) {
            for (const std::string& name : leftovers.value()) {
                if (name.size() > 4 && (name.compare(name.size() - 4, 4, ".tmp") == 0 ||
                                        name.compare(name.size() - 4, 4, ".new") == 0)) {
                    const Status removed_leftover = detail::fs::remove_file(impl->generations / name);
                    if (!removed_leftover) {
                        return removed_leftover.error();
                    }
                }
            }
        }
    } else {
        impl->report.current_epoch = StoreEpoch(current.epoch);
        impl->report.recovered_generation = state.generation;
    }

    impl->published.store(std::make_shared<const StoreState>(std::move(state)));
    impl->open = true;
    Store store;
    store.impl_ = std::move(impl);
    return store;
}

Store::Store() noexcept = default;
Store::~Store() { close(); }
Store::Store(Store&& other) noexcept : impl_(std::move(other.impl_)) {}
Store& Store::operator=(Store&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool Store::is_open() const noexcept { return impl_ != nullptr && impl_->open; }

bool Store::is_writable() const noexcept { return impl_ != nullptr && impl_->open && impl_->writable; }

StoreEpoch Store::epoch() const noexcept {
    if (impl_ == nullptr) {
        return StoreEpoch{};
    }
    const StoreStatePtr state = impl_->published.load();
    return state == nullptr ? StoreEpoch{} : state->epoch;
}

GenerationSequence Store::generation() const noexcept {
    if (impl_ == nullptr) {
        return GenerationSequence{};
    }
    const StoreStatePtr state = impl_->published.load();
    return state == nullptr ? GenerationSequence{} : state->generation;
}

const RecoveryReport& Store::recovery() const noexcept {
    static const RecoveryReport empty;
    return impl_ == nullptr ? empty : impl_->report;
}

const std::filesystem::path& Store::root() const noexcept {
    static const std::filesystem::path empty;
    return impl_ == nullptr ? empty : impl_->root;
}

StoreStatePtr Store::state() const noexcept { return impl_ == nullptr ? nullptr : impl_->published.load(); }

GenerationSequence Store::next_sequence() const noexcept {
    const StoreStatePtr current = state();
    if (current == nullptr) {
        return GenerationSequence{};
    }
    const auto next = current->generation.next();
    return next.has_value() ? *next : GenerationSequence{};
}

Outcome<CommitResult> Store::commit(const CommitRequest& request) {
    if (impl_ == nullptr || !impl_->open) {
        return Error(ErrorCode::StoreClosed, "the store is not open");
    }
    if (!impl_->writable) {
        return Error(ErrorCode::StoreReadOnly, "the store was opened read-only");
    }
    if (request.idempotency_key.empty()) {
        return Error(ErrorCode::InvalidInput,
                     "a commit must carry an idempotency key so that a retry can be recognised");
    }
    const std::lock_guard<std::mutex> guard(impl_->commit_mutex);
    const StoreStatePtr current = impl_->published.load();
    if (current == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store has no published state");
    }
    if (request.expected_epoch != current->epoch) {
        return Error(ErrorCode::StaleStoreEpoch, "the commit names a store epoch that is no longer current")
            .with_context("expected", request.expected_epoch.to_string())
            .with_context("current", current->epoch.to_string());
    }
    if (request.sequence <= current->generation) {
        // A sequence that has already been used is either a retry or a replay of
        // a dead attempt. Both are answered from the durable retry record, which
        // was written in the same commit as the mutation it describes.
        for (const RetryRecord& record : current->retries) {
            if (record.sequence != request.sequence) {
                continue;
            }
            if (record.idempotency_key != request.idempotency_key) {
                return Error(ErrorCode::IdempotencyConflict,
                             "this sequence was already committed under a different idempotency key")
                    .with_context("sequence", request.sequence.to_string());
            }
            CommitResult result;
            result.generation = record.resulting_generation;
            result.replayed = true;
            return result;
        }
        return Error(ErrorCode::StaleMutationSequence, "the commit sequence does not advance the store")
            .with_context("requested", request.sequence.to_string())
            .with_context("current", current->generation.to_string());
    }
    const auto expected = current->generation.next();
    if (!expected.has_value() || request.sequence != *expected) {
        return Error(ErrorCode::StaleMutationSequence, "a commit must claim exactly the next sequence")
            .with_context("requested", request.sequence.to_string())
            .with_context("expected", expected.has_value() ? expected->to_string() : std::string("exhausted"));
    }

    // Apply the mutation to a copy. Validation, application and publication are
    // one step: a refused commit leaves the published state exactly as it was.
    StoreState next = *current;
    next.generation = request.sequence;
    next.previous_generation = current->generation;
    next.previous_payload_digest = impl_->current_payload_digest;
    next.recovered = false;
    CommitResult result;
    result.generation = request.sequence;

    std::map<AssetRefId, std::size_t> per_asset;
    for (const EvidenceRecord& record : next.evidence) {
        ++per_asset[record.subject().asset()];
    }
    for (const EvidenceRecord& record : request.mutation.append_evidence) {
        if (record.subject().asset().is_nil()) {
            return Error(ErrorCode::MalformedAssetReference, "an evidence record must name the asset it observed");
        }
        if (per_asset.find(record.subject().asset()) == per_asset.end() && per_asset.size() >= impl_->options.max_assets) {
            return Error(ErrorCode::AssetCapacityExceeded, "the store holds as many assets as it is configured to")
                .with_context("bound", std::to_string(impl_->options.max_assets));
        }
        std::size_t& count = per_asset[record.subject().asset()];
        if (count >= impl_->options.max_evidence_per_asset) {
            return Error(ErrorCode::EvidenceCapacityExceeded,
                         "the asset holds as many evidence records as the store is configured to keep")
                .with_context("bound", std::to_string(impl_->options.max_evidence_per_asset));
        }
        ++count;
        next.evidence.push_back(record.with_commit_epoch(current->epoch));
        ++result.appended_evidence;
    }
    std::sort(next.evidence.begin(), next.evidence.end(), [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
        return EvidenceRecord::canonical_less(lhs, rhs);
    });
    if (next.evidence.size() > limits::kMaxEvidenceTotal) {
        return Error(ErrorCode::EvidenceCapacityExceeded, "the store holds as many evidence records as it may");
    }

    for (const HistoryEntry& entry : request.mutation.publish_history) {
        HistoryEntry value = entry;
        value.revision = next_history_revision(*current, entry.asset, entry.generation, entry.revision);
        next.history.push_back(std::move(value));
    }
    if (impl_->options.max_history_per_asset != 0) {
        std::map<AssetRefId, std::size_t> history_per_asset;
        std::vector<HistoryEntry> trimmed;
        trimmed.reserve(next.history.size());
        // Keep the newest entries per asset, which is what a bounded history
        // means: the oldest published answers are the ones dropped, and the
        // count of published revisions is not rewritten.
        for (auto iterator = next.history.rbegin(); iterator != next.history.rend(); ++iterator) {
            std::size_t& count = history_per_asset[iterator->asset];
            if (count >= impl_->options.max_history_per_asset) {
                continue;
            }
            ++count;
            trimmed.push_back(*iterator);
        }
        std::reverse(trimmed.begin(), trimmed.end());
        std::sort(trimmed.begin(), trimmed.end(), [](const HistoryEntry& lhs, const HistoryEntry& rhs) {
            return HistoryEntry::canonical_less(lhs, rhs);
        });
        next.history = std::move(trimmed);
    }

    for (const RefusalRecord& refusal : request.mutation.append_refusals) {
        next.refusals.push_back(refusal);
    }
    if (next.refusals.size() > impl_->options.max_refusals) {
        const std::size_t excess = next.refusals.size() - impl_->options.max_refusals;
        next.refusals.erase(next.refusals.begin(), next.refusals.begin() + static_cast<std::ptrdiff_t>(excess));
    }

    for (const SourceStreamState& update : request.mutation.update_streams) {
        bool replaced = false;
        for (SourceStreamState& existing : next.source_streams) {
            if (existing.source == update.source) {
                existing = update;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (next.source_streams.size() >= impl_->options.max_source_streams) {
                return Error(ErrorCode::SourceCapacityExceeded,
                             "the store holds as many source streams as it is configured to");
            }
            next.source_streams.push_back(update);
        }
    }
    std::sort(next.source_streams.begin(), next.source_streams.end(),
              [](const SourceStreamState& lhs, const SourceStreamState& rhs) { return lhs.source < rhs.source; });

    RetryRecord retry;
    retry.sequence = request.sequence;
    retry.idempotency_key = request.idempotency_key;
    retry.resulting_generation = request.sequence;
    {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        retry.committed_at = Instant(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }
    next.retries.push_back(retry);
    if (next.retries.size() > impl_->options.max_retry_records) {
        const std::size_t excess = next.retries.size() - impl_->options.max_retry_records;
        next.retries.erase(next.retries.begin(), next.retries.begin() + static_cast<std::ptrdiff_t>(excess));
    }

    const std::uint64_t generation_value = request.sequence.value();
    const std::uint64_t retained = impl_->options.retained_generations;
    // The boundary names the oldest generation that stays, so the number left on
    // disk is exactly the configured retention.
    next.pruned_below =
        GenerationSequence(generation_value >= retained ? generation_value - retained + 1 : 0);

    detail::GenerationHeader header;
    header.format_version = limits::kStoreFormatVersion;
    header.generation = generation_value;
    header.epoch = current->epoch.value();
    header.store_id = next.store_id;
    header.previous_generation = current->generation.value();
    header.previous_payload_digest = impl_->current_payload_digest;
    header.pruned_below = next.pruned_below.value();
    header.recorded_at_ns = retry.committed_at.unix_nanoseconds();

    const std::vector<std::uint8_t> payload = detail::encode_payload(next, header);
    header.payload_length = payload.size();
    header.payload_crc = detail::crc32_bytes(payload.data(), payload.size());
    const std::vector<std::uint8_t> file_bytes = detail::encode_generation(next, header);
    const std::uint64_t digest = detail::payload_digest(payload);

    const std::filesystem::path generation_file =
        impl_->generations / detail::generation_file_name(request.sequence);
    const Status written = detail::fs::write_new(generation_file, file_bytes);
    if (!written) {
        return written.error();
    }

    detail::CurrentRecord commit_point;
    commit_point.format_version = limits::kStoreFormatVersion;
    commit_point.store_id = next.store_id;
    commit_point.generation = generation_value;
    commit_point.epoch = current->epoch.value();
    commit_point.payload_digest = digest;
    const Status published = detail::fs::write_atomic(impl_->root / kCurrentName, detail::encode_current(commit_point));
    if (!published) {
        return published.error();
    }
    // The commit point is durable here. Everything above this line is
    // recoverable; everything below is bookkeeping that a crash may skip without
    // changing what the store says.

    if (impl_->options.retained_generations > 0) {
        const auto present = list_generations(impl_->generations);
        for (const GenerationSequence& generation : present) {
            // Only generations below the retention boundary are removed: the
            // boundary names the oldest generation still kept, so everything at
            // or above it stays.
            if (generation >= next.pruned_below) {
                continue;
            }
            const Status removed =
                detail::fs::remove_file(impl_->generations / detail::generation_file_name(generation));
            if (!removed) {
                return removed.error();
            }
        }
    }

    impl_->current_payload_digest = digest;
    impl_->published.store(std::make_shared<const StoreState>(std::move(next)));
    return result;
}

Outcome<StoreAudit> Store::audit() const {
    if (impl_ == nullptr) {
        return Error(ErrorCode::StoreClosed, "the store is not open");
    }
    return audit_at(impl_->root);
}

Outcome<StoreAudit> Store::audit_at(const std::filesystem::path& root) {
    StoreAudit audit;
    const auto meta_path = root / kMetaName;
    std::vector<std::uint8_t> bytes;
    const auto meta_exists = detail::fs::file_exists(meta_path);
    if (!meta_exists) {
        return meta_exists.error();
    }
    if (!meta_exists.value()) {
        audit.problems.emplace_back("the store meta file is absent, so this directory is not a store");
        return audit;
    }
    audit.found = true;
    const auto meta_read = detail::fs::read_file(meta_path, 4096, bytes);
    if (!meta_read) {
        return meta_read.error();
    }
    detail::MetaRecord meta;
    const Status meta_status = detail::decode_meta(bytes, meta);
    if (!meta_status) {
        audit.problems.emplace_back("the store meta file is unreadable: " + meta_status.error().to_string());
        return audit;
    }
    audit.store_id = format_uuid_text(meta.store_id);
    audit.format_version = StoreFormatVersion(meta.format_version);

    const auto current_path = root / kCurrentName;
    detail::CurrentRecord current;
    const auto current_read = detail::fs::read_file(current_path, 4096, bytes);
    if (!current_read) {
        return current_read.error();
    }
    if (!current_read.value()) {
        audit.problems.emplace_back("the commit point is absent");
    } else {
        const Status current_status = detail::decode_current(bytes, current);
        if (!current_status) {
            audit.problems.emplace_back("the commit point is unreadable: " + current_status.error().to_string());
        } else {
            audit.current_generation = GenerationSequence(current.generation);
            audit.current_generation_present = GenerationSequence(current.generation);
            audit.epoch = StoreEpoch(current.epoch);
            if (current.store_id != meta.store_id) {
                audit.problems.emplace_back("the commit point names a different store than the meta file");
            }
        }
    }

    const std::filesystem::path generations = generations_directory(root);
    audit.generations = list_generations(generations);
    for (const GenerationSequence& generation : audit.generations) {
        const std::filesystem::path file = generations / detail::generation_file_name(generation);
        StoreState state;
        detail::GenerationHeader header;
        const Status loaded = load_generation_file(file, state, header);
        if (loaded) {
            audit.valid_generations.push_back(generation);
            if (generation == GenerationSequence(current.generation)) {
                audit.evidence_records = state.evidence.size();
                audit.history_records = state.history.size();
                audit.retry_records = state.retries.size();
                if (current.payload_digest != detail::payload_digest(detail::encode_payload(state, header))) {
                    audit.problems.emplace_back("the commit point digest does not match generation " +
                                                generation.to_string());
                }
                if (state.store_id != meta.store_id) {
                    audit.problems.emplace_back("generation " + generation.to_string() +
                                                " belongs to a different store");
                }
            }
            continue;
        }
        if (loaded.error().code() == ErrorCode::TornTailDiscarded) {
            audit.torn_generations.push_back(generation);
            audit.problems.emplace_back("generation " + generation.to_string() + " is torn: " +
                                        loaded.error().message());
        } else {
            audit.corrupt_generations.push_back(generation);
            audit.problems.emplace_back("generation " + generation.to_string() + " is corrupt: " +
                                        loaded.error().to_string());
        }
    }
    for (const GenerationSequence& generation : audit.generations) {
        if (generation > GenerationSequence(current.generation)) {
            audit.uncommitted_generations.push_back(generation);
            audit.problems.emplace_back("generation " + generation.to_string() +
                                        " is above the commit point and was never committed");
        }
    }
    if (audit.current_generation_present.has_value()) {
        for (std::uint64_t value = 1; value < current.generation; ++value) {
            const GenerationSequence candidate(value);
            if (std::find(audit.generations.begin(), audit.generations.end(), candidate) != audit.generations.end()) {
                continue;
            }
            // A generation that was pruned is not missing; the commit point of
            // the generation that pruned it records the boundary.
            if (!audit.valid_generations.empty()) {
                bool pruned = false;
                for (const GenerationSequence& valid : audit.valid_generations) {
                    if (valid > candidate) {
                        const std::filesystem::path file = generations / detail::generation_file_name(valid);
                        StoreState state;
                        detail::GenerationHeader header;
                        if (load_generation_file(file, state, header) && header.pruned_below >= value) {
                            pruned = true;
                            break;
                        }
                    }
                }
                if (pruned) {
                    continue;
                }
            }
            audit.missing_generations.push_back(candidate);
            audit.problems.emplace_back("generation " + candidate.to_string() +
                                        " is missing and was not pruned by any later generation");
        }
    }
    audit.ok = audit.problems.empty();
    return audit;
}

void Store::close() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->open && impl_->writable) {
        const std::lock_guard<std::mutex> guard(impl_->commit_mutex);
        const std::vector<std::uint8_t> marker{'c', 'l', 'e', 'a', 'n', '\n'};
        // The clean marker is written while the write lock is still held, so a
        // second writer cannot interpret a half-closed session as a clean one.
        const Status written = detail::fs::write_atomic(impl_->root / kCleanName, marker);
        (void)written;
    }
    impl_->open = false;
    impl_->lock.release();
    impl_.reset();
}

}  // namespace asset_health
