// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The durable store.
//
// Layout, and the exact commit point:
//
//   <root>/meta                     format magic, store identifier, creation time
//   <root>/LOCK                     held for the whole lifetime of a writable open
//   <root>/CLEAN                    present only between a clean close and the next open
//   <root>/CURRENT                  the commit point: the generation a reader must load
//   <root>/generations/gen-<n>.ahg  immutable generations, newest highest
//
// A commit writes a complete generation payload to a temporary name, flushes it,
// renames it into place, and only then replaces CURRENT atomically. CURRENT is
// therefore the commit point: before it moves, the commit does not exist; after
// it moves, the commit is durable. A generation file that CURRENT does not name
// is an uncommitted artifact of an interrupted commit, and it is never adopted
// as state. That rule is what keeps recovered evidence from being promoted to
// current evidence.

#ifndef ASSET_HEALTH_PERSISTENCE_HPP
#define ASSET_HEALTH_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asset_health/error.hpp"
#include "asset_health/evidence.hpp"
#include "asset_health/history.hpp"
#include "asset_health/limits.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// What this observatory remembers about one producer stream, so that a replayed
/// or out-of-order delivery is detected rather than admitted twice.
struct SourceStreamState {
    SourceId source;
    SourceKind kind = SourceKind::TelemetryFeed;
    StreamEpoch epoch{};
    StreamSequence last_sequence{};
    /// Digest of the last admitted statement from this stream in this epoch.
    std::uint64_t last_digest = 0;
    /// Identifier of the last admitted statement, so a repeated delivery can be
    /// answered with the record it repeats.
    EvidenceId last_evidence_id{};
    Instant last_observed_at{};
    Instant last_received_at{};
};

/// One durable retry record: the identity of a commit, kept inside the same
/// commit the retry would replay, so a retry that arrives after a crash is
/// answered from durable state rather than applied twice.
struct RetryRecord {
    GenerationSequence sequence{};
    std::string idempotency_key;
    GenerationSequence resulting_generation{};
    Instant committed_at{};
};

/// The immutable state a store publishes. A reader holds a shared pointer to one
/// of these and never blocks a writer.
struct ASSET_HEALTH_API StoreState {
    StoreEpoch epoch{};
    GenerationSequence generation{};
    /// Identity of the store directory this state was written by.
    detail::UuidBytes store_id{};
    /// The generation this one superseded, and the digest of its payload, so an
    /// audit can tell a pruned generation from a lost one.
    GenerationSequence previous_generation{};
    std::uint64_t previous_payload_digest = 0;
    /// Generations below this one have been pruned from disk.
    GenerationSequence pruned_below{};
    /// Canonical order.
    std::vector<EvidenceRecord> evidence;
    /// Canonical order, newest last.
    std::vector<HistoryEntry> history;
    std::vector<RefusalRecord> refusals;
    std::vector<RetryRecord> retries;
    std::vector<SourceStreamState> source_streams;
    /// True when this state was reconstructed by recovery rather than by a
    /// sequence of commits in this process.
    bool recovered = false;

    /// The newest stream epoch seen from each source.
    [[nodiscard]] std::map<SourceId, StreamEpoch> newest_source_epochs() const;
    /// Records for one asset, canonical order, optionally restricted to one
    /// generation.
    [[nodiscard]] std::vector<EvidenceRecord> evidence_for(const AssetRefId& asset) const;
    [[nodiscard]] const SourceStreamState* stream_state(const SourceId& source) const;
};

using StoreStatePtr = std::shared_ptr<const StoreState>;

/// A change to be committed as one atomic step.
struct Mutation {
    std::vector<EvidenceRecord> append_evidence;
    std::vector<HistoryEntry> publish_history;
    std::vector<RefusalRecord> append_refusals;
    std::vector<SourceStreamState> update_streams;
    /// True when the mutation is a no-op that still advances the store epoch,
    /// used by a writable open to publish ownership. Never set by callers.
    bool epoch_only = false;
};

struct CommitRequest {
    /// The epoch the caller believes it is writing under. A stale epoch is
    /// refused: the writer is fenced out, including across a restart.
    StoreEpoch expected_epoch{};
    /// The sequence this commit claims. It must be exactly one greater than the
    /// last committed generation, which makes the sequence gap-free and makes a
    /// replay detectable.
    GenerationSequence sequence{};
    /// Identity of the attempt. A repeated (sequence, key) replays the recorded
    /// outcome instead of applying the mutation again; a repeated sequence with a
    /// different key is refused as a conflict.
    std::string idempotency_key;
    Mutation mutation;
};

struct CommitResult {
    GenerationSequence generation{};
    /// True when the commit was answered from a durable retry record rather than
    /// applied now.
    bool replayed = false;
    /// Number of statements added by this commit.
    std::size_t appended_evidence = 0;
};

struct StoreOptions {
    std::filesystem::path root;
    /// Create the store when the directory is absent or empty.
    bool create_if_missing = true;
    /// Open without the write lock and without publishing an epoch. A read-only
    /// open always observes exactly one committed generation, because generation
    /// files are immutable and CURRENT is replaced atomically.
    bool read_only = false;
    std::size_t max_evidence_per_asset = limits::kMaxEvidencePerAsset;
    std::size_t max_assets = limits::kMaxAssets;
    std::size_t max_history_per_asset = limits::kMaxHistoryPerAsset;
    std::size_t max_refusals = 256;
    std::size_t max_retry_records = limits::kMaxRetryRecords;
    std::size_t max_source_streams = limits::kMaxSources;
    /// Generations kept on disk after a successful commit. Older ones are
    /// removed; the store records the boundary so an audit can tell a pruned
    /// generation from a missing one.
    std::size_t retained_generations = 8;
};

/// What happened when the store was opened.
struct ASSET_HEALTH_API RecoveryReport {
    bool created = false;
    bool existing = false;
    /// True when the previous session closed cleanly.
    bool previous_close_clean = false;
    /// True when the newest committed generation could not be read and an older
    /// one was used.
    bool torn_tail_discarded = false;
    /// Generations found on disk that CURRENT did not name. They were never
    /// committed, so none of them was adopted; each is listed here.
    std::vector<GenerationSequence> uncommitted_discarded;
    /// Generations lost to corruption below the recovered one, reported so the
    /// loss is visible even though it does not stop the open.
    std::vector<GenerationSequence> damaged_below_recovered;
    StoreEpoch previous_epoch{};
    StoreEpoch current_epoch{};
    GenerationSequence recovered_generation{};
    std::uint64_t evidence_records = 0;
    std::uint64_t history_records = 0;
    /// Dynamic statements (telemetry, faults) whose epoch is older than the
    /// current one, and which therefore may not count as fresh evidence.
    std::uint64_t dynamic_records_demoted = 0;
    std::string detail;

    [[nodiscard]] std::string to_string() const;
    /// True when any state was reconstructed rather than read from a clean
    /// close.
    [[nodiscard]] bool recovered() const noexcept {
        return torn_tail_discarded || !uncommitted_discarded.empty() || previous_close_clean == false;
    }
};

/// A verdict on the files in a store directory, produced without opening the
/// store for writing.
struct ASSET_HEALTH_API StoreAudit {
    enum class FileVerdict : std::uint8_t { Valid = 0, Torn = 1, Corrupt = 2, Unreadable = 3, Uncommitted = 4 };

    bool ok = false;
    bool found = false;
    std::string store_id;
    StoreFormatVersion format_version{};
    GenerationSequence current_generation{};
    std::optional<GenerationSequence> current_generation_present;
    StoreEpoch epoch{};
    std::vector<GenerationSequence> generations;
    std::vector<GenerationSequence> valid_generations;
    std::vector<GenerationSequence> torn_generations;
    std::vector<GenerationSequence> corrupt_generations;
    std::vector<GenerationSequence> uncommitted_generations;
    std::uint64_t evidence_records = 0;
    std::uint64_t history_records = 0;
    std::uint64_t retry_records = 0;
    /// Missing generations below the recovered one that were not pruned.
    std::vector<GenerationSequence> missing_generations;
    /// Human-readable problems, in a deterministic order. Empty when ok.
    std::vector<std::string> problems;

    [[nodiscard]] std::string to_string() const;
};

/// One writable session on a store directory.
class ASSET_HEALTH_API Store {
public:
    Store() noexcept;
    ~Store();
    Store(Store&& other) noexcept;
    Store& operator=(Store&& other) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// Opens a store. A writable open takes the single-writer lock, recovers the
    /// newest committed generation, and publishes a strictly greater epoch. A
    /// read-only open takes no lock and publishes nothing.
    [[nodiscard]] static Outcome<Store> open(const StoreOptions& options);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_writable() const noexcept;
    [[nodiscard]] StoreEpoch epoch() const noexcept;
    [[nodiscard]] GenerationSequence generation() const noexcept;
    [[nodiscard]] const RecoveryReport& recovery() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    /// The published snapshot. Lock-free: a reader never blocks a writer and a
    /// writer never waits for a reader.
    [[nodiscard]] StoreStatePtr state() const noexcept;

    /// The sequence the next commit must claim.
    [[nodiscard]] GenerationSequence next_sequence() const noexcept;

    /// Commits one mutation. Validation, application to a copy, and publication
    /// are one step: a refused commit leaves the published state exactly as it
    /// was.
    [[nodiscard]] Outcome<CommitResult> commit(const CommitRequest& request);

    /// Audits the files in this store's directory without changing anything.
    [[nodiscard]] Outcome<StoreAudit> audit() const;

    /// Audits a store directory without opening it for writing.
    [[nodiscard]] static Outcome<StoreAudit> audit_at(const std::filesystem::path& root);

    /// Marks the session closed cleanly and releases the lock. Idempotent.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// True when the payload of a generation file is intact, by header and payload
/// checksum. Exposed so that a test can corrupt a byte and assert the verdict
/// rather than reimplementing the format.
[[nodiscard]] ASSET_HEALTH_API Outcome<GenerationSequence> inspect_generation_file(
    const std::filesystem::path& file, std::uint64_t* payload_bytes = nullptr);

}  // namespace asset_health

#endif  // ASSET_HEALTH_PERSISTENCE_HPP
