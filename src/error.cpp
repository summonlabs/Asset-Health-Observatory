// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/error.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace asset_health {
namespace {

/// The canonical name of every error code, indexed by code. A missing entry
/// would make error_code_name return an empty string, which the round-trip test
/// treats as a defect, so the table is complete by construction and checked by a
/// static assertion below.
constexpr std::array<std::string_view, kErrorCodeCount> kErrorCodeNames{
    "none",
    "invalid_input",
    "malformed_evidence_id",
    "malformed_asset_reference",
    "malformed_source_id",
    "malformed_component_id",
    "malformed_token",
    "malformed_version_text",
    "malformed_text",
    "malformed_timestamp",
    "malformed_quantity",
    "malformed_duration",
    "empty_required_field",
    "text_too_long",
    "unknown_metric",
    "unknown_unit",
    "unknown_evidence_kind",
    "unit_mismatch",
    "value_out_of_range",
    "invalid_time_order",
    "arithmetic_overflow",
    "inconsistent_payload",
    "unsupported_evidence",
    "invalid_policy",
    "invalid_limit",
    "invalid_configuration",
    "invalid_rational",
    "division_by_zero",
    "authority_domain_violation",
    "synthetic_provenance_required",
    "duplicate_evidence_id",
    "evidence_conflict",
    "stale_source_epoch",
    "replayed_source_sequence",
    "future_dated_observation",
    "evidence_capacity_exceeded",
    "unknown_asset",
    "unknown_generation",
    "superseded_generation",
    "evidence_not_admitted",
    "evidence_rejected",
    "asset_capacity_exceeded",
    "source_capacity_exceeded",
    "history_capacity_exceeded",
    "finding_capacity_exceeded",
    "payload_too_large",
    "invalid_evidence_binding",
    "assessment_unavailable",
    "no_evidence",
    "store_open_failed",
    "store_not_found",
    "store_corrupt",
    "store_version_unsupported",
    "store_integrity_failed",
    "store_locked",
    "store_io_error",
    "store_layout_invalid",
    "store_recovery_impossible",
    "store_publish_failed",
    "store_closed",
    "store_already_open",
    "torn_tail_discarded",
    "interior_corruption",
    "stale_store_epoch",
    "stale_mutation_sequence",
    "idempotency_conflict",
    "commit_point_missing",
    "store_read_only",
    "unsupported_format_version",
    "payload_checksum_mismatch",
    "header_checksum_mismatch",
    "record_decode_failed",
    "recovered_state_not_current",
    "cancelled",
    "shutting_down",
    "already_closed",
    "queue_full",
    "ticket_unknown",
    "worker_failure",
    "deadlock_detected",
    "concurrent_mutation",
    "publication_race",
    "timeout_not_supported",
    "invalid_state",
    "operation_refused",
    "internal_invariant_violation",
    "allocation_failed",
};

static_assert(kErrorCodeNames.size() == kErrorCodeCount,
              "the error code name table must name every code exactly once");

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    const auto index = static_cast<std::size_t>(code);
    if (index >= kErrorCodeNames.size()) {
        return "unknown_code";
    }
    return kErrorCodeNames[index];
}

std::optional<ErrorCode> error_code_from_name(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kErrorCodeNames.size(); ++index) {
        if (kErrorCodeNames[index] == name) {
            return static_cast<ErrorCode>(index);
        }
    }
    return std::nullopt;
}

bool is_input_error(ErrorCode code) noexcept {
    const auto value = static_cast<std::uint16_t>(code);
    return value >= 1 && value <= 29;
}

bool is_staleness_error(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StaleSourceEpoch:
        case ErrorCode::ReplayedSourceSequence:
        case ErrorCode::SupersededGeneration:
        case ErrorCode::StaleStoreEpoch:
        case ErrorCode::StaleMutationSequence:
        case ErrorCode::RecoveredStateNotCurrent:
        case ErrorCode::FutureDatedObservation:
            return true;
        default:
            return false;
    }
}

bool is_storage_error(ErrorCode code) noexcept {
    const auto value = static_cast<std::uint16_t>(code);
    return value >= 49 && value <= 72;
}

bool is_capacity_error(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::EvidenceCapacityExceeded:
        case ErrorCode::AssetCapacityExceeded:
        case ErrorCode::SourceCapacityExceeded:
        case ErrorCode::HistoryCapacityExceeded:
        case ErrorCode::FindingCapacityExceeded:
        case ErrorCode::PayloadTooLarge:
        case ErrorCode::QueueFull:
            return true;
        default:
            return false;
    }
}

bool is_boundary_error(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::AuthorityDomainViolation:
        case ErrorCode::SyntheticProvenanceRequired:
        case ErrorCode::UnsupportedEvidence:
        case ErrorCode::SupersededGeneration:
        case ErrorCode::EvidenceRejected:
            return true;
        default:
            return false;
    }
}

Error::Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

Error Error::with_context(std::string key, std::string value) const {
    Error copy = *this;
    copy.context_ = std::make_pair(std::move(key), std::move(value));
    return copy;
}

std::string Error::to_string() const {
    std::string text(error_code_name(code_));
    text.append(": ");
    text.append(message_);
    if (context_.has_value()) {
        text.append(" (");
        text.append(context_->first);
        text.push_back('=');
        text.append(context_->second);
        text.push_back(')');
    }
    return text;
}

}  // namespace asset_health
