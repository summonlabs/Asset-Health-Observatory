// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Published history: what this observatory said, in the order it said it.
//
// An entry is a digest of a published assessment, not the assessment itself, and
// it is durable. History is what makes a change of answer visible: a state that
// moved between two evaluations of the same evidence is a defect, and the
// history is where that shows up.

#ifndef ASSET_HEALTH_HISTORY_HPP
#define ASSET_HEALTH_HISTORY_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "asset_health/assessment.hpp"
#include "asset_health/error.hpp"
#include "asset_health/identity.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// One published assessment, reduced to what a history needs.
struct ASSET_HEALTH_API HistoryEntry {
    AssessmentRevision revision{};
    AssetRefId asset{};
    AssetGeneration generation{};
    HealthState state = HealthState::Unknown;
    AssessmentFlags flags{};
    RiskBand risk_band = RiskBand::Undetermined;
    Instant evaluated_at{};
    /// When the entry was committed. Distinct from evaluated_at: an assessment
    /// can be published for a past instant.
    Instant recorded_at{};
    std::string policy_fingerprint;
    std::uint16_t finding_count = 0;
    std::uint16_t attention_findings = 0;
    std::uint32_t evidence_depended_on = 0;

    /// Canonical order: by asset, then generation, then revision. Total.
    [[nodiscard]] static bool canonical_less(const HistoryEntry& lhs, const HistoryEntry& rhs) noexcept;
    [[nodiscard]] std::string to_string() const;
};

/// A change of reported state between two consecutive entries for one asset.
struct HealthTransition {
    AssessmentRevision from_revision{};
    AssessmentRevision to_revision{};
    HealthState from = HealthState::Unknown;
    HealthState to = HealthState::Unknown;
    Instant at{};
};

/// A history listing for one asset.
struct ASSET_HEALTH_API HistoryView {
    AssetRefId asset{};
    AssetGeneration generation{};
    /// Newest first.
    std::vector<HistoryEntry> entries;
    /// Oldest first.
    std::vector<HealthTransition> transitions;
    /// Number of entries retained for this asset, including any that did not fit.
    std::size_t total_published = 0;
    /// True when the listing was cut by the configured history bound.
    bool truncated = false;

    [[nodiscard]] std::string to_string() const;
};

/// Reduces a published assessment to a history entry.
[[nodiscard]] ASSET_HEALTH_API HistoryEntry make_history_entry(const HealthAssessment& assessment,
                                                               Instant recorded_at);

/// Builds the listing for one asset and generation from the durable entries,
/// newest first, with the transitions between consecutive states oldest first.
[[nodiscard]] ASSET_HEALTH_API HistoryView build_history(const std::vector<HistoryEntry>& entries,
                                                         const AssetRefId& asset, AssetGeneration generation);

}  // namespace asset_health

#endif  // ASSET_HEALTH_HISTORY_HPP
