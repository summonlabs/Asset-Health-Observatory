// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The evaluator: the one function that turns admitted evidence into a health
// assessment.
//
// It is a pure function of (evidence, policy, evaluation instant, store epoch,
// observed stream epochs). It reads no clock, takes no lock, mutates nothing and
// consults no global state, which is what makes an assessment reproducible from
// the evidence set alone.

#ifndef ASSET_HEALTH_EVALUATION_HPP
#define ASSET_HEALTH_EVALUATION_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <vector>

#include "asset_health/assessment.hpp"
#include "asset_health/error.hpp"
#include "asset_health/evidence.hpp"
#include "asset_health/policy.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// Everything the evaluator is told about the world outside the evidence set.
struct EvaluationContext {
    /// The instant the assessment is about. Supplied by the caller.
    Instant now{};
    /// Epoch of the store the evidence was read from. Dynamic evidence committed
    /// in an earlier epoch is recovered, and recovered dynamic evidence is not
    /// fresh evidence.
    StoreEpoch store_epoch{};
    /// The newest stream epoch seen from each source in this store epoch. A
    /// source that restarted has a higher epoch, and everything it said before
    /// the restart belongs to a dead epoch of that stream.
    std::map<SourceId, StreamEpoch> live_source_epochs;
    /// True when the store was reopened from durable state and the previous run
    /// did not close cleanly. Reported so a consumer can tell a normal restart
    /// from a recovery.
    bool store_recovered = false;
};

/// What the evaluator is asked about.
struct EvaluationRequest {
    /// The asset to assess. Evidence for other assets is ignored.
    AssetRefId asset{};
    /// The incarnation to assess. Evidence bound to another generation of the
    /// same identity is preserved and reported as superseded, never mixed in.
    AssetGeneration generation{};
    /// Every admitted statement about this asset, in any order. The evaluator
    /// sorts them into the canonical order itself.
    std::vector<EvidenceRecord> evidence;
    /// Statements refused at admission that named this asset.
    std::vector<RefusalRecord> refusals;
};

/// Evaluates one asset.
///
/// The result is a total function of the request, the policy and the context:
/// the same inputs produce byte-identical findings, ordering, dependency list
/// and risk arithmetic. Never fails for a well-formed request; an empty evidence
/// set produces an Unknown assessment with the NoEvidence flag rather than an
/// error, because "we have observed nothing" is an answer, not a failure.
[[nodiscard]] ASSET_HEALTH_API HealthAssessment evaluate(const EvaluationRequest& request, const HealthPolicy& policy,
                                                         const EvaluationContext& context);

/// Convenience overload that reports a malformed request (a nil asset) as a
/// rejection instead of assessing the nil identity.
[[nodiscard]] ASSET_HEALTH_API Outcome<HealthAssessment> evaluate_checked(const EvaluationRequest& request,
                                                                          const HealthPolicy& policy,
                                                                          const EvaluationContext& context);

}  // namespace asset_health

#endif  // ASSET_HEALTH_EVALUATION_HPP
