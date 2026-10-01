// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Documents. Every public answer has one canonical text and one canonical JSON
// rendering, so a consumer can diff two answers and a test can assert on them.

#ifndef ASSET_HEALTH_EXPORT_HPP
#define ASSET_HEALTH_EXPORT_HPP

#include <string>

#include "asset_health/assessment.hpp"
#include "asset_health/evidence.hpp"
#include "asset_health/history.hpp"
#include "asset_health/observatory.hpp"
#include "asset_health/persistence.hpp"
#include "asset_health/policy.hpp"

namespace asset_health {

/// Canonical JSON of one assessment, including every finding, degradation
/// indicator, dependency and risk input.
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const HealthAssessment& assessment);
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const HistoryView& history);
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const EvidenceRecord& record);
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const RefusalRecord& refusal);
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const RecoveryReport& report);
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const StoreAudit& audit);
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const IngestOutcome& outcome);
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const HealthPolicy& policy);

/// Canonical JSON of a list of evidence records, in the order given.
[[nodiscard]] ASSET_HEALTH_API std::string to_json(const std::vector<EvidenceRecord>& records);

/// Canonical JSON of a store statistics summary: asset count, evidence count,
/// source count, generation, epoch.
[[nodiscard]] ASSET_HEALTH_API std::string statistics_json(const StoreState& state);

/// Canonical JSON of the source streams the store remembers.
[[nodiscard]] ASSET_HEALTH_API std::string sources_json(const StoreState& state);

}  // namespace asset_health

#endif  // ASSET_HEALTH_EXPORT_HPP
