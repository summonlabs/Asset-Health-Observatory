// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Every configured bound in one place. The observatory never allocates from a
// count it has not first checked against the remaining input and against the
// limits below, so a hostile or corrupt document cannot make it allocate
// without bound.

#ifndef ASSET_HEALTH_LIMITS_HPP
#define ASSET_HEALTH_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace asset_health::limits {

/// Longest canonical token (source identifiers, component identifiers, fault
/// codes, metric-independent labels).
inline constexpr std::size_t kMaxTokenLength = 64;

/// Longest version or baseline text.
inline constexpr std::size_t kMaxVersionTextLength = 64;

/// Longest free-form detail text carried by a finding, risk input or audit note.
inline constexpr std::size_t kMaxDetailTextLength = 512;

/// Longest human label (fault description, maintenance reference).
inline constexpr std::size_t kMaxLabelTextLength = 128;

/// Number of evidence records retained for one asset. Reaching it refuses the
/// ingest with EvidenceCapacityExceeded rather than evicting silently: an
/// observatory that forgets evidence cannot explain its own findings.
inline constexpr std::size_t kMaxEvidencePerAsset = 4096;

/// Number of distinct assets the store will hold.
inline constexpr std::size_t kMaxAssets = 4096;

/// Number of distinct evidence sources the store will hold.
inline constexpr std::size_t kMaxSources = 512;

/// Number of published assessments retained per asset.
inline constexpr std::size_t kMaxHistoryPerAsset = 512;

/// Number of findings carried by one assessment. A finding beyond this bound is
/// folded into a summary finding that names the count, so a truncated
/// explanation is never presented as a complete one.
inline constexpr std::size_t kMaxFindingsPerAssessment = 256;

/// Number of evidence dependencies listed by one assessment.
inline constexpr std::size_t kMaxDependenciesPerAssessment = 1024;

/// Number of telemetry samples retained per (asset, generation, metric) for trend
/// analysis. Older samples stay in the evidence set but stop contributing to a
/// degradation indicator.
inline constexpr std::size_t kMaxTrendSamples = 256;

/// Number of risk inputs the risk model may declare.
inline constexpr std::size_t kMaxRiskInputs = 32;

/// Number of distinct degradation indicators one assessment reports.
inline constexpr std::size_t kMaxDegradationIndicators = 64;

/// Number of evidence records the whole store will hold.
inline constexpr std::size_t kMaxEvidenceTotal = 1000000;

/// Number of history entries the whole store will hold.
inline constexpr std::size_t kMaxHistoryTotal = 100000;

/// Number of refusal records the whole store will hold.
inline constexpr std::size_t kMaxRefusalsTotal = 4096;

/// Number of source stream records the whole store will hold.
inline constexpr std::size_t kMaxStreamsTotal = 512;

/// Longest single durable generation payload in bytes.
inline constexpr std::uint64_t kMaxGenerationPayloadBytes = 512ull * 1024ull * 1024ull;

/// Longest canonical evidence/serialised identifier text.
inline constexpr std::size_t kMaxIdentifierTextLength = 36;

/// Longest store path accepted by the command line.
inline constexpr std::size_t kMaxPathTextLength = 4096;

/// Retry/idempotency records retained durably. A retry older than the retained
/// window is rejected as a stale sequence rather than replayed.
inline constexpr std::size_t kMaxRetryRecords = 1024;

/// Number of ingest workers the observatory will start.
inline constexpr std::size_t kMaxIngestWorkers = 8;

/// Depth of one ingest queue.
inline constexpr std::size_t kMaxIngestQueueDepth = 65536;

/// Number of queued tickets retained for outcome reporting.
inline constexpr std::size_t kMaxRetainedTickets = 4096;

/// Bytes of durable generation file header.
inline constexpr std::size_t kGenerationHeaderBytes = 128;

/// Durable store format version implemented by this build.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

/// Oldest durable store format version this build can read.
inline constexpr std::uint32_t kMinReadableStoreFormatVersion = 1;

/// Canonical assessment document schema version.
inline constexpr std::uint32_t kAssessmentSchemaVersion = 1;

}  // namespace asset_health::limits

#endif  // ASSET_HEALTH_LIMITS_HPP
