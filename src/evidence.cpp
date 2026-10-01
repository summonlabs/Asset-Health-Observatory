// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/evidence.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace asset_health {
namespace {

[[nodiscard]] int class_rank(SourceClass klass) noexcept {
    switch (klass) {
        case SourceClass::Authoritative:
            return 0;
        case SourceClass::Peer:
            return 1;
        case SourceClass::Synthetic:
            return 2;
    }
    return 3;
}

}  // namespace

const EvidenceId& EvidenceId::nil() noexcept {
    static const EvidenceId value;
    return value;
}

EvidenceId EvidenceId::from_bytes(const bytes_type& bytes) noexcept {
    EvidenceId value;
    value.bytes_ = bytes;
    return value;
}

Outcome<EvidenceId> EvidenceId::generate() {
    bytes_type bytes{};
    if (!generate_uuid_v4(bytes)) {
        return Error(ErrorCode::AllocationFailed,
                     "the platform entropy source is unavailable, so no identifier was generated");
    }
    return EvidenceId::from_bytes(bytes);
}

std::optional<EvidenceId> EvidenceId::parse(std::string_view text) noexcept {
    const auto bytes = parse_uuid_text(text);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return EvidenceId::from_bytes(*bytes);
}

bool EvidenceId::is_nil() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string EvidenceId::to_string() const { return format_uuid_text(bytes_); }

Outcome<EvidenceRecord> EvidenceRecord::make(Parts parts) {
    if (parts.id.is_nil()) {
        return Error(ErrorCode::MalformedEvidenceId, "an evidence record must carry a generated identifier");
    }
    if (parts.subject.asset().is_nil()) {
        return Error(ErrorCode::MalformedAssetReference, "an evidence record must name the asset it observed");
    }
    if (parts.subject.generation().is_zero()) {
        return Error(ErrorCode::InvalidEvidenceBinding,
                     "an evidence record must name the incarnation it observed");
    }
    if (parts.provenance.source().empty()) {
        return Error(ErrorCode::MalformedSourceId, "an evidence record must name its source");
    }
    if (parts.provenance.is_synthetic() != (parts.provenance.kind() == SourceKind::SyntheticPlant)) {
        // Modelled evidence must be labelled as modelled, and a synthetic
        // producer must not present itself as a measurement. Both directions of
        // that rule are enforced here, because either one alone is a way to get
        // modelled numbers counted as measured ones.
        return Error(ErrorCode::SyntheticProvenanceRequired,
                     "a synthetic source and a synthetic class must be declared together");
    }
    if (!may_speak_for(parts.provenance.kind(), owning_domain(parts.payload.kind))) {
        return Error(ErrorCode::AuthorityDomainViolation,
                     "the source does not speak for the domain that owns this kind of evidence")
            .with_context("source_kind", std::string(asset_health::to_string(parts.provenance.kind())))
            .with_context("evidence_kind", std::string(asset_health::to_string(parts.payload.kind)))
            .with_context("owning_domain",
                          std::string(asset_health::to_string(owning_domain(parts.payload.kind))));
    }
    if (const auto problem = validate_payload(parts.payload); problem.has_value()) {
        return *problem;
    }
    if (parts.payload.kind == EvidenceKind::LifecycleStatement &&
        parts.payload.lifecycle.effective_at.has_value() &&
        *parts.payload.lifecycle.effective_at > parts.provenance.observed_at()) {
        return Error(ErrorCode::InvalidTimeOrder,
                     "a lifecycle state cannot take effect after the instant it was observed");
    }

    EvidenceRecord record;
    record.id_ = std::move(parts.id);
    record.subject_ = std::move(parts.subject);
    record.provenance_ = std::move(parts.provenance);
    record.payload_ = std::move(parts.payload);
    return record;
}

EvidenceRecord EvidenceRecord::with_commit_epoch(StoreEpoch epoch) const noexcept {
    EvidenceRecord copy = *this;
    copy.commit_epoch_ = epoch;
    return copy;
}

std::string EvidenceRecord::fact_key() const {
    switch (payload_.kind) {
        case EvidenceKind::IdentityStatement:
            return "identity";
        case EvidenceKind::LifecycleStatement:
            return "lifecycle";
        case EvidenceKind::MaintenanceStatement:
            return "maintenance";
        case EvidenceKind::FirmwareStatement:
            if (payload_.firmware.component.empty()) {
                return "firmware";
            }
            return "firmware:" + payload_.firmware.component.str();
        case EvidenceKind::TelemetryReading:
            return "telemetry:" + std::string(asset_health::to_string(payload_.telemetry.metric));
        case EvidenceKind::FaultStatement:
            return "fault:" + payload_.fault.component.str() + ":" + payload_.fault.code.str();
    }
    return "unknown";
}

bool EvidenceRecord::kinds_can_conflict(EvidenceKind lhs, EvidenceKind rhs) noexcept {
    if (lhs != rhs) {
        return false;
    }
    switch (lhs) {
        case EvidenceKind::IdentityStatement:
        case EvidenceKind::LifecycleStatement:
        case EvidenceKind::FirmwareStatement:
        case EvidenceKind::TelemetryReading:
            return true;
        case EvidenceKind::MaintenanceStatement:
            // Maintenance statements describe windows that can legitimately
            // overlap: two work orders on one asset is not a contradiction.
            return false;
        case EvidenceKind::FaultStatement:
            // Two fault statements about the same component and code refine one
            // another; the newer statement wins and the older is superseded.
            return false;
    }
    return false;
}

bool EvidenceRecord::canonical_less(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept {
    if (lhs.subject_ != rhs.subject_) {
        return lhs.subject_ < rhs.subject_;
    }
    if (lhs.observed_at() != rhs.observed_at()) {
        return lhs.observed_at() < rhs.observed_at();
    }
    if (lhs.payload_.kind != rhs.payload_.kind) {
        return lhs.payload_.kind < rhs.payload_.kind;
    }
    const std::string lhs_key = lhs.fact_key();
    const std::string rhs_key = rhs.fact_key();
    if (lhs_key != rhs_key) {
        return lhs_key < rhs_key;
    }
    if (lhs.provenance_.source() != rhs.provenance_.source()) {
        return lhs.provenance_.source() < rhs.provenance_.source();
    }
    if (lhs.provenance_.epoch() != rhs.provenance_.epoch()) {
        return lhs.provenance_.epoch() < rhs.provenance_.epoch();
    }
    if (lhs.provenance_.sequence() != rhs.provenance_.sequence()) {
        return lhs.provenance_.sequence() < rhs.provenance_.sequence();
    }
    return lhs.id_ < rhs.id_;
}

bool EvidenceRecord::has_precedence_over(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept {
    const int lhs_rank = class_rank(lhs.provenance_.klass());
    const int rhs_rank = class_rank(rhs.provenance_.klass());
    if (lhs_rank != rhs_rank) {
        return lhs_rank < rhs_rank;
    }
    if (lhs.observed_at() != rhs.observed_at()) {
        return rhs.observed_at() < lhs.observed_at();
    }
    if (lhs.provenance_.epoch() != rhs.provenance_.epoch()) {
        return rhs.provenance_.epoch() < lhs.provenance_.epoch();
    }
    if (lhs.provenance_.sequence() != rhs.provenance_.sequence()) {
        return rhs.provenance_.sequence() < lhs.provenance_.sequence();
    }
    if (lhs.provenance_.source() != rhs.provenance_.source()) {
        return lhs.provenance_.source() < rhs.provenance_.source();
    }
    return lhs.id_ < rhs.id_;
}

std::string EvidenceRecord::to_string() const {
    std::string text;
    text.append(id_.to_string());
    text.append(" ");
    text.append(subject_.asset().to_string());
    text.append("@g");
    text.append(subject_.generation().to_string());
    text.push_back(' ');
    text.append(asset_health::to_string(payload_.kind));
    text.push_back(' ');
    text.append(fact_key());
    text.append(" from ");
    text.append(provenance_.to_string());
    return text;
}

std::vector<EvidenceRecord> canonical_order(std::vector<EvidenceRecord> records) {
    std::sort(records.begin(), records.end(),
              [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) { return EvidenceRecord::canonical_less(lhs, rhs); });
    return records;
}

const std::vector<EvidenceKind>& all_evidence_kinds() {
    static const std::vector<EvidenceKind> kinds{
        EvidenceKind::IdentityStatement,  EvidenceKind::LifecycleStatement, EvidenceKind::MaintenanceStatement,
        EvidenceKind::FirmwareStatement,  EvidenceKind::TelemetryReading,   EvidenceKind::FaultStatement,
    };
    return kinds;
}

}  // namespace asset_health
