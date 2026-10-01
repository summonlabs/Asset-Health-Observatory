// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The evidence record: one statement, by one producer, about one incarnation of
// one asset, at one time, with the provenance needed to judge it.
//
// A record is immutable once admitted. Everything this observatory concludes is
// a function of records, and every conclusion names the records it used, so an
// answer can always be traced back to the statements that produced it.

#ifndef ASSET_HEALTH_EVIDENCE_HPP
#define ASSET_HEALTH_EVIDENCE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_health/error.hpp"
#include "asset_health/identity.hpp"
#include "asset_health/observation.hpp"
#include "asset_health/provenance.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// Identity of one admitted evidence record: a 128-bit canonical identifier.
/// The identifier is the handle every assessment explanation quotes; it is
/// generated once, when the record is admitted, and never changes.
class ASSET_HEALTH_API EvidenceId {
public:
    using bytes_type = detail::UuidBytes;

    [[nodiscard]] static const EvidenceId& nil() noexcept;
    [[nodiscard]] static EvidenceId from_bytes(const bytes_type& bytes) noexcept;

    /// Generates a version-4 identifier from the process entropy source. Reports
    /// AllocationFailed when the platform entropy source is unavailable instead
    /// of substituting a predictable value.
    [[nodiscard]] static Outcome<EvidenceId> generate();

    [[nodiscard]] static std::optional<EvidenceId> parse(std::string_view text) noexcept;

    [[nodiscard]] const bytes_type& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_nil() const noexcept;
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend bool operator==(const EvidenceId& lhs, const EvidenceId& rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }
    [[nodiscard]] friend bool operator!=(const EvidenceId& lhs, const EvidenceId& rhs) noexcept {
        return !(lhs == rhs);
    }
    [[nodiscard]] friend bool operator<(const EvidenceId& lhs, const EvidenceId& rhs) noexcept {
        return lhs.bytes_ < rhs.bytes_;
    }

private:
    bytes_type bytes_{};
};

/// One admitted statement.
class ASSET_HEALTH_API EvidenceRecord {
public:
    /// The parts a producer supplies. The commit epoch is assigned by the store
    /// when the record becomes durable, so it is absent here by construction.
    struct Parts {
        EvidenceId id;
        EvidenceSubject subject;
        Provenance provenance;
        EvidencePayload payload;
    };

    /// Validates and builds a record. Rejects a nil identifier, an invalid
    /// subject, a payload inconsistent with its kind, a synthetic claim that is
    /// not backed by a synthetic source, a lifecycle statement that would place
    /// an asset in service before it exists, and a maintenance window whose end
    /// is not after its start.
    [[nodiscard]] static Outcome<EvidenceRecord> make(Parts parts);

    [[nodiscard]] const EvidenceId& id() const noexcept { return id_; }
    [[nodiscard]] const EvidenceSubject& subject() const noexcept { return subject_; }
    [[nodiscard]] const Provenance& provenance() const noexcept { return provenance_; }
    [[nodiscard]] const EvidencePayload& payload() const noexcept { return payload_; }
    [[nodiscard]] StoreEpoch commit_epoch() const noexcept { return commit_epoch_; }

    [[nodiscard]] EvidenceKind kind() const noexcept { return payload_.kind; }
    [[nodiscard]] Instant observed_at() const noexcept { return provenance_.observed_at(); }
    [[nodiscard]] Instant received_at() const noexcept { return provenance_.received_at(); }
    [[nodiscard]] const SourceId& source() const noexcept { return provenance_.source(); }
    [[nodiscard]] SourceKind source_kind() const noexcept { return provenance_.kind(); }
    [[nodiscard]] SourceClass source_class() const noexcept { return provenance_.klass(); }
    [[nodiscard]] bool is_synthetic() const noexcept { return provenance_.is_synthetic(); }

    /// True once the record has been written to durable state. A record read
    /// from a recovered store is always committed; one held by a caller that has
    /// not yet ingested it is not.
    [[nodiscard]] bool is_committed() const noexcept { return !commit_epoch_.is_zero(); }

    /// The epoch the record became durable in. This is what makes "evidence
    /// recovered from a previous run" distinguishable from evidence this run
    /// admitted, which is what stops recovered dynamic evidence from counting as
    /// fresh evidence.
    [[nodiscard]] EvidenceRecord with_commit_epoch(StoreEpoch epoch) const noexcept;

    /// What this statement is about, independent of who said it and when. Two
    /// records with the same key are two statements about the same fact, which is
    /// what makes precedence and conflict detection possible. The key is
    /// canonical and stable: "identity", "lifecycle", "maintenance",
    /// "firmware:<component>", "telemetry:<metric>", "fault:<component>:<code>".
    [[nodiscard]] std::string fact_key() const;

    /// Whether two records can contradict each other. Fault statements about the
    /// same key and maintenance windows about the same asset refine an
    /// understanding rather than contradicting one another, so they are not
    /// conflicts; identity, lifecycle, firmware and telemetry statements are.
    [[nodiscard]] static bool kinds_can_conflict(EvidenceKind lhs, EvidenceKind rhs) noexcept;

    /// Canonical enumeration order: by asset, then generation, then observation
    /// time, then evidence kind, then fact key, then source, then stream
    /// sequence, then identifier. Total, so a listing is reproducible byte for
    /// byte.
    [[nodiscard]] static bool canonical_less(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept;

    /// Precedence between two statements about the same fact: an authoritative
    /// source outranks a peer, which outranks a synthetic one; then the newer
    /// observation wins; then the higher stream epoch; then the higher sequence;
    /// then the lexicographically smaller source identifier. Total, so "which
    /// statement wins" never depends on insertion order.
    [[nodiscard]] static bool has_precedence_over(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept;

    [[nodiscard]] std::string to_string() const;

private:
    EvidenceId id_{};
    EvidenceSubject subject_{};
    Provenance provenance_{};
    EvidencePayload payload_{};
    StoreEpoch commit_epoch_{};
};

/// A statement that was offered and not admitted, kept so that an assessment can
/// report that the evidence it saw is not everything the producers sent. A
/// refusal is not evidence: it never contributes to a health state, and it is
/// never rewritten into one.
struct RefusalRecord {
    /// The asset the statement claimed, when the claim was well formed enough to
    /// name one.
    std::optional<AssetRefId> asset;
    AssetGeneration generation{};
    SourceId source;
    SourceKind source_kind = SourceKind::TelemetryFeed;
    /// The kind the statement claimed to be, when it said.
    std::optional<EvidenceKind> claimed_kind;
    /// Why it was refused.
    ErrorCode code = ErrorCode::None;
    /// One line naming the specific problem, for the audit trail.
    std::string detail;
    Instant received_at{};
};

/// Sorts records into the canonical order.
[[nodiscard]] ASSET_HEALTH_API std::vector<EvidenceRecord> canonical_order(std::vector<EvidenceRecord> records);

/// The evidence kinds, in the fixed order the durable format and every listing
/// use.
[[nodiscard]] ASSET_HEALTH_API const std::vector<EvidenceKind>& all_evidence_kinds();

}  // namespace asset_health

#endif  // ASSET_HEALTH_EVIDENCE_HPP
