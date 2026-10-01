// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The evaluator.
//
// One pass over the admitted evidence for one asset and incarnation, producing a
// state, the findings that support it, the degradation it derived, the exact
// statements it depended on, and an explicitly justified risk view.
//
// The whole function is deterministic. Every collection is ordered by a total
// order before it is used, no clock is read, and every threshold comes from the
// policy that is passed in and fingerprinted into the answer.

#include "asset_health/evaluation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "asset_health/limits.hpp"

namespace asset_health {
namespace {

[[nodiscard]] int class_rank(SourceClass klass) noexcept { return static_cast<int>(klass); }

/// The tolerance two readings of one metric may differ by before they are called
/// contradictory.
[[nodiscard]] std::int64_t conflict_tolerance(const MetricPolicy& policy, std::int64_t lhs, std::int64_t rhs) noexcept {
    const std::int64_t lhs_magnitude = lhs < 0 ? (lhs == std::numeric_limits<std::int64_t>::min()
                                                      ? std::numeric_limits<std::int64_t>::max()
                                                      : -lhs)
                                               : lhs;
    const std::int64_t rhs_magnitude = rhs < 0 ? (rhs == std::numeric_limits<std::int64_t>::min()
                                                      ? std::numeric_limits<std::int64_t>::max()
                                                      : -rhs)
                                               : rhs;
    const std::int64_t magnitude = std::max(lhs_magnitude, rhs_magnitude);
    const auto relative = checked_mul_add(policy.conflict_relative_ppm, magnitude, 0);
    std::int64_t relative_tolerance = policy.conflict_absolute_milli;
    if (relative.has_value()) {
        relative_tolerance = std::max(relative_tolerance, *relative / 1000000);
    } else {
        relative_tolerance = std::numeric_limits<std::int64_t>::max();
    }
    return std::max(policy.conflict_absolute_milli, relative_tolerance);
}

enum class Excursion : std::uint8_t { None = 0, Warning = 1, Critical = 2 };

[[nodiscard]] Excursion classify_excursion(const MetricPolicy& policy, std::int64_t milli) noexcept {
    if (policy.critical_high.has_value() && milli > *policy.critical_high) {
        return Excursion::Critical;
    }
    if (policy.critical_low.has_value() && milli < *policy.critical_low) {
        return Excursion::Critical;
    }
    if (policy.warn_high.has_value() && milli > *policy.warn_high) {
        return Excursion::Warning;
    }
    if (policy.warn_low.has_value() && milli < *policy.warn_low) {
        return Excursion::Warning;
    }
    return Excursion::None;
}

/// One admitted statement with the freshness this evaluator judged it to have.
struct Candidate {
    EvidenceRecord record;
    FreshnessResult freshness;
};

/// The evaluator's working state for one asset.
class Evaluation {
public:
    Evaluation(const EvaluationRequest& request, const HealthPolicy& policy, const EvaluationContext& context)
        : request_(request), policy_(policy), context_(context) {}

    [[nodiscard]] HealthAssessment run();

private:
    // --- collection helpers -------------------------------------------------
    void add_finding(FindingCode code, FindingSeverity severity, HealthState impact, FindingDisposition disposition,
                     std::string detail);
    Finding& last_finding();
    void note(EvidenceId id);
    void note(const std::vector<EvidenceId>& ids);
    void note_record(const EvidenceRecord& record);
    void contribute(HealthState state);
    void set_flag(AssessmentFlag flag);
    void raise_finding_limit();

    [[nodiscard]] const EvidenceRecord* winner_of(const std::vector<const Candidate*>& group) const;
    [[nodiscard]] FreshnessReason reason_of(const Candidate& candidate) const;

    // --- phases -------------------------------------------------------------
    void assess_identity();
    void assess_lifecycle();
    void assess_maintenance();
    void assess_faults();
    void assess_telemetry();
    void assess_firmware();
    void assess_degradation();
    void assess_source_loss();
    void assess_refusals();
    void assess_other_generations();
    void compute_risk();

    // --- members ------------------------------------------------------------
    const EvaluationRequest& request_;
    const HealthPolicy& policy_;
    const EvaluationContext& context_;

    std::vector<Candidate> current_;
    std::vector<EvidenceRecord> other_generation_;
    HealthAssessment assessment_;
    AssessmentFlags flags_;
    HealthState floor_ = HealthState::Healthy;
    bool lifecycle_end_of_life_ = false;
    bool lifecycle_usable_ = false;
    std::optional<MaintenanceObservation> mask_;
    std::string mask_description_;
    std::vector<EvidenceId> dependency_ids_;
    std::vector<std::pair<const Candidate*, Freshness>> dependency_details_;
    std::size_t maintained_metric_count_ = 0;
    std::size_t excursion_count_ = 0;
    std::size_t worsening_count_ = 0;
    std::size_t corrective_maintenance_count_ = 0;
    std::size_t distinct_fault_keys_ = 0;
    std::size_t recent_fault_keys_ = 0;
    std::int64_t max_active_fault_rank_ = 0;
    bool firmware_known_ = false;
    bool firmware_compliant_ = false;
    std::optional<Instant> service_entry_;
    bool findings_truncated_ = false;
    std::size_t suppressed_findings_ = 0;
    /// The last finding that was actually recorded, and a scratch finding that
    /// absorbs the follow-up work of one that was dropped at the bound.
    Finding* last_written_ = nullptr;
    Finding discarded_;
};

void Evaluation::raise_finding_limit() {
    ++suppressed_findings_;
    findings_truncated_ = true;
}

void Evaluation::add_finding(FindingCode code, FindingSeverity severity, HealthState impact,
                             FindingDisposition disposition, std::string detail) {
    if (assessment_.findings.size() >= limits::kMaxFindingsPerAssessment) {
        raise_finding_limit();
        // The finding was not recorded, and the caller's follow-up work -- naming
        // the component, attaching the statements -- must not land on whichever
        // finding happens to be last. last_finding() returns a scratch finding
        // instead, so a dropped finding cannot mis-attribute evidence to another
        // one.
        last_written_ = nullptr;
        return;
    }
    Finding finding;
    finding.code = code;
    finding.severity = severity;
    finding.impact = impact;
    finding.disposition = disposition;
    finding.detail = std::move(detail);
    assessment_.findings.push_back(std::move(finding));
    last_written_ = &assessment_.findings.back();
}

Finding& Evaluation::last_finding() { return last_written_ != nullptr ? *last_written_ : discarded_; }

void Evaluation::note(EvidenceId id) {
    if (std::find(dependency_ids_.begin(), dependency_ids_.end(), id) == dependency_ids_.end()) {
        dependency_ids_.push_back(id);
    }
}

void Evaluation::note(const std::vector<EvidenceId>& ids) {
    for (const EvidenceId& id : ids) {
        note(id);
    }
}

void Evaluation::note_record(const EvidenceRecord& record) {
    note(record.id());
    for (const auto& entry : dependency_details_) {
        // An entry with no candidate is a statement that was not admitted (a
        // refusal, or another incarnation). It has no freshness to report, and
        // reading through it would be reading through a null pointer.
        if (entry.first != nullptr && entry.first->record.id() == record.id()) {
            return;
        }
    }
    // The dependency detail carries the freshness the statement had when this
    // assessment was made, which is what makes "why did this count" answerable
    // without re-running the evaluator.
    for (const Candidate& candidate : current_) {
        if (candidate.record.id() == record.id()) {
            dependency_details_.emplace_back(&candidate, candidate.freshness.state);
            return;
        }
    }
    dependency_details_.emplace_back(nullptr, Freshness::Stale);
}

void Evaluation::contribute(HealthState state) { floor_ = worse_of(floor_, state); }

void Evaluation::set_flag(AssessmentFlag flag) { flags_.set(flag); }

const EvidenceRecord* Evaluation::winner_of(const std::vector<const Candidate*>& group) const {
    const EvidenceRecord* best = nullptr;
    for (const Candidate* candidate : group) {
        if (best == nullptr || EvidenceRecord::has_precedence_over(candidate->record, *best)) {
            best = &candidate->record;
        }
    }
    return best;
}

FreshnessReason Evaluation::reason_of(const Candidate& candidate) const { return candidate.freshness.reason; }

HealthAssessment Evaluation::run() {
    assessment_.asset = request_.asset;
    assessment_.generation = request_.generation;
    assessment_.evaluated_at = context_.now;
    assessment_.policy_id = policy_.id().str();
    assessment_.policy_version = policy_.version();
    assessment_.policy_fingerprint = policy_.fingerprint();

    for (const EvidenceRecord& record : request_.evidence) {
        if (record.subject().asset() != request_.asset) {
            continue;
        }
        if (record.subject().generation() != request_.generation) {
            other_generation_.push_back(record);
            continue;
        }
        Candidate candidate;
        candidate.record = record;
        candidate.freshness =
            evaluate_freshness(record, context_.now, policy_.freshness(), context_.store_epoch,
                               context_.live_source_epochs, context_.store_recovered);
        current_.push_back(std::move(candidate));
    }
    std::sort(current_.begin(), current_.end(), [](const Candidate& lhs, const Candidate& rhs) {
        return EvidenceRecord::canonical_less(lhs.record, rhs.record);
    });
    assessment_.evidence_considered = current_.size();

    assess_other_generations();
    assess_refusals();

    if (current_.empty()) {
        set_flag(AssessmentFlag::NoEvidence);
        add_finding(FindingCode::NoEvidenceForAsset, FindingSeverity::Warning, HealthState::Unknown,
                    FindingDisposition::Supported,
                    "no admitted statement names this asset and incarnation, so nothing is known about it");
        contribute(HealthState::Unknown);
        assessment_.state = HealthState::Unknown;
        assessment_.flags = flags_;
        compute_risk();
        assessment_.flags = flags_;
        return assessment_;
    }

    assess_identity();
    assess_lifecycle();
    assess_maintenance();
    assess_faults();
    assess_telemetry();
    assess_firmware();
    assess_degradation();
    assess_source_loss();

    if (findings_truncated_) {
        set_flag(AssessmentFlag::Indeterminate);
        add_finding(FindingCode::FindingLimitReached, FindingSeverity::Warning, HealthState::Unknown,
                    FindingDisposition::Uncertain,
                    "the explanation reached its bound of " + std::to_string(limits::kMaxFindingsPerAssessment) +
                        " findings and " + std::to_string(suppressed_findings_) +
                        " further findings are summarised here rather than listed");
        contribute(HealthState::Unknown);
    }

    compute_risk();

    if (lifecycle_end_of_life_) {
        // Retired is terminal: the observations above are still reported, but the
        // service-health question is not the one an end-of-life asset answers.
        floor_ = HealthState::Retired;
    }
    assessment_.state = floor_;
    assessment_.flags = flags_;

    std::sort(assessment_.findings.begin(), assessment_.findings.end(),
              [](const Finding& lhs, const Finding& rhs) { return Finding::canonical_less(lhs, rhs); });

    // Dependencies: every statement the explanation named, with its freshness.
    std::vector<EvidenceDependency> dependencies;
    dependencies.reserve(dependency_details_.size());
    for (const auto& entry : dependency_details_) {
        EvidenceDependency dependency;
        if (entry.first != nullptr) {
            dependency.id = entry.first->record.id();
            dependency.kind = entry.first->record.kind();
            dependency.source = entry.first->record.source();
            dependency.source_kind = entry.first->record.source_kind();
            dependency.source_class = entry.first->record.source_class();
            dependency.observed_at = entry.first->record.observed_at();
            dependency.freshness = entry.first->freshness.state;
            dependency.fresh = entry.first->freshness.usable;
            dependency.synthetic = entry.first->record.is_synthetic();
        } else {
            dependency.freshness = entry.second;
            dependency.fresh = is_usable(entry.second);
        }
        dependencies.push_back(std::move(dependency));
    }
    std::sort(dependencies.begin(), dependencies.end(),
              [](const EvidenceDependency& lhs, const EvidenceDependency& rhs) {
                  if (lhs.kind != rhs.kind) {
                      return lhs.kind < rhs.kind;
                  }
                  if (lhs.source != rhs.source) {
                      return lhs.source < rhs.source;
                  }
                  if (lhs.observed_at != rhs.observed_at) {
                      return lhs.observed_at < rhs.observed_at;
                  }
                  return lhs.id < rhs.id;
              });
    if (dependencies.size() > limits::kMaxDependenciesPerAssessment) {
        set_flag(AssessmentFlag::EvidenceListedPartially);
        dependencies.resize(limits::kMaxDependenciesPerAssessment);
    }
    assessment_.evidence_depended_on = dependency_ids_.size();
    assessment_.dependencies = std::move(dependencies);
    std::sort(assessment_.degradation.begin(), assessment_.degradation.end(),
              [](const DegradationIndicator& lhs, const DegradationIndicator& rhs) {
                  return static_cast<int>(lhs.metric) < static_cast<int>(rhs.metric);
              });
    assessment_.flags = flags_;
    return assessment_;
}

void Evaluation::assess_other_generations() {
    if (other_generation_.empty()) {
        return;
    }
    bool newer = false;
    for (const EvidenceRecord& record : other_generation_) {
        if (record.subject().generation() > request_.generation) {
            newer = true;
        }
        set_flag(AssessmentFlag::IdentityReplaced);
    }
    std::string detail = std::to_string(other_generation_.size()) +
                         " statement(s) name another incarnation of this identity and are excluded from this "
                         "assessment; an observation about one incarnation is never an observation about another";
    if (newer) {
        detail.append("; at least one names a later incarnation than the one assessed");
        contribute(HealthState::Unknown);
    }
    add_finding(FindingCode::GenerationSuperseded, FindingSeverity::Warning, HealthState::Unknown,
                FindingDisposition::Superseded, std::move(detail));
    std::size_t listed = 0;
    for (const EvidenceRecord& record : other_generation_) {
        if (listed >= 32) {
            break;
        }
        last_finding().evidence.push_back(record.id());
        note_record(record);
        ++listed;
    }
}

void Evaluation::assess_refusals() {
    std::size_t matching = 0;
    for (const RefusalRecord& refusal : request_.refusals) {
        if (!refusal.asset.has_value() || *refusal.asset != request_.asset) {
            continue;
        }
        if (matching >= 16) {
            break;
        }
        ++matching;
        set_flag(AssessmentFlag::RefusedEvidence);
        add_finding(FindingCode::EvidenceRefused, FindingSeverity::Warning, HealthState::Healthy,
                    FindingDisposition::Refused,
                    "a statement from source " + refusal.source.str() + " was refused with " +
                        std::string(error_code_name(refusal.code)) + ": " + refusal.detail);
    }
    if (matching != 0) {
        contribute(HealthState::Unknown);
    }
}

void Evaluation::assess_identity() {
    std::vector<const Candidate*> identities;
    std::vector<AssetGeneration> generations;
    for (const Candidate& candidate : current_) {
        if (candidate.record.kind() != EvidenceKind::IdentityStatement) {
            continue;
        }
        identities.push_back(&candidate);
        generations.push_back(candidate.record.subject().generation());
    }
    if (identities.empty()) {
        add_finding(FindingCode::IdentityNotObserved, FindingSeverity::Warning, HealthState::Healthy,
                    FindingDisposition::Informational,
                    "no identity statement has been admitted for this asset, so the incarnation and revision "
                    "assessed here are the ones the other statements name");
        return;
    }
    const EvidenceRecord* winner = winner_of(identities);
    if (winner == nullptr) {
        return;
    }
    assessment_.identity_revision = winner->payload().identity.revision;
    std::size_t stale = 0;
    for (const Candidate* candidate : identities) {
        if (!candidate->freshness.usable) {
            ++stale;
        }
        note_record(candidate->record);
    }
    if (stale == identities.size()) {
        set_flag(AssessmentFlag::StaleEvidence);
        add_finding(FindingCode::IdentityNotObserved, FindingSeverity::Warning, HealthState::Unknown,
                    FindingDisposition::Uncertain,
                    "every identity statement for this asset is outside the static freshness window");
        contribute(HealthState::Unknown);
        return;
    }
    add_finding(FindingCode::IdentityObserved, FindingSeverity::Info, HealthState::Healthy,
                FindingDisposition::Informational,
                "identity revision " + winner->payload().identity.revision.to_string() + " observed from " +
                    winner->source().str());
    last_finding().evidence.push_back(winner->id());
    // Two statements of the same class that name different revisions are a
    // contradiction: neither can be preferred without inventing a rule.
    std::vector<const Candidate*> same_class;
    for (const Candidate* candidate : identities) {
        if (candidate->freshness.usable &&
            class_rank(candidate->record.source_class()) == class_rank(winner->source_class())) {
            same_class.push_back(candidate);
        }
    }
    for (const Candidate* candidate : same_class) {
        if (candidate->record.payload().identity.revision != winner->payload().identity.revision) {
            set_flag(AssessmentFlag::ConflictingEvidence);
            set_flag(AssessmentFlag::Indeterminate);
            add_finding(FindingCode::MultipleIdentityClaims, FindingSeverity::Major, HealthState::Unknown,
                        FindingDisposition::Uncertain,
                        "two sources of the same class name different identity revisions (" +
                            winner->payload().identity.revision.to_string() + " and " +
                            candidate->record.payload().identity.revision.to_string() + ")");
            last_finding().evidence.push_back(winner->id());
            last_finding().evidence.push_back(candidate->record.id());
            note(candidate->record.id());
            contribute(HealthState::Unknown);
            break;
        }
    }
    if (generations.size() > 1) {
        set_flag(AssessmentFlag::IdentityReplaced);
    }
}

void Evaluation::assess_lifecycle() {
    std::vector<const Candidate*> statements;
    for (const Candidate& candidate : current_) {
        if (candidate.record.kind() == EvidenceKind::LifecycleStatement) {
            statements.push_back(&candidate);
        }
    }
    if (statements.empty()) {
        set_flag(AssessmentFlag::UnknownLifecycle);
        add_finding(FindingCode::LifecycleNotObserved, FindingSeverity::Warning, policy_.unknown_lifecycle_cap(),
                    FindingDisposition::Supported,
                    "no lifecycle statement has been admitted for this asset, so its service state is unknown and "
                    "health cannot be asserted as healthy");
        contribute(policy_.unknown_lifecycle_cap());
        return;
    }
    std::vector<const Candidate*> usable;
    std::size_t stale = 0;
    std::size_t future = 0;
    for (const Candidate* candidate : statements) {
        note_record(candidate->record);
        if (candidate->freshness.usable) {
            usable.push_back(candidate);
        } else if (candidate->freshness.state == Freshness::FutureDated) {
            ++future;
        } else {
            ++stale;
        }
    }
    if (usable.empty()) {
        set_flag(AssessmentFlag::UnknownLifecycle);
        set_flag(AssessmentFlag::StaleEvidence);
        const FindingCode code =
            future > stale ? FindingCode::StaticEvidenceFutureDated : FindingCode::StaticEvidenceStale;
        add_finding(code, FindingSeverity::Warning, policy_.unknown_lifecycle_cap(),
                    FindingDisposition::Uncertain,
                    "no lifecycle statement for this asset is inside its freshness window (" +
                        std::to_string(stale) + " stale, " + std::to_string(future) +
                        " future-dated), so the service state is unknown");
        contribute(policy_.unknown_lifecycle_cap());
        return;
    }
    const EvidenceRecord* winner = winner_of(usable);
    if (winner == nullptr) {
        return;
    }
    lifecycle_usable_ = true;
    const LifecycleState state = winner->payload().lifecycle.state;
    if (winner->payload().lifecycle.effective_at.has_value()) {
        service_entry_ = winner->payload().lifecycle.effective_at;
    }
    // Cross-source disagreement of the same class.
    for (const Candidate* candidate : usable) {
        if (candidate->record.id() == winner->id()) {
            continue;
        }
        if (class_rank(candidate->record.source_class()) != class_rank(winner->source_class())) {
            continue;
        }
        if (candidate->record.payload().lifecycle.state == state) {
            continue;
        }
        set_flag(AssessmentFlag::ConflictingEvidence);
        set_flag(AssessmentFlag::Indeterminate);
        add_finding(FindingCode::LifecycleConflicting, FindingSeverity::Major, HealthState::Unknown,
                    FindingDisposition::Uncertain,
                    "two sources of the same class disagree about the lifecycle state: " +
                        std::string(to_string(state)) + " and " +
                        std::string(to_string(candidate->record.payload().lifecycle.state)));
        last_finding().evidence.push_back(winner->id());
        last_finding().evidence.push_back(candidate->record.id());
        contribute(HealthState::Unknown);
        break;
    }

    const ServiceClass klass = service_class(state);
    const std::string observed = "lifecycle state " + std::string(to_string(state)) + " observed from " +
                                 winner->source().str() + " (class " + std::string(to_string(klass)) + ")";
    switch (klass) {
        case ServiceClass::InService:
            add_finding(FindingCode::AssetInService, FindingSeverity::Info, HealthState::Healthy,
                        FindingDisposition::Informational, observed + "; the asset is expected to be doing useful work");
            break;
        case ServiceClass::InServiceImpaired:
            add_finding(FindingCode::AssetServiceImpaired, FindingSeverity::Warning, HealthState::Degraded,
                        FindingDisposition::Supported,
                        observed + "; the lifecycle authority has acknowledged impaired service");
            contribute(HealthState::Degraded);
            break;
        case ServiceClass::ServiceWithheld: {
            const HealthState impact = state == LifecycleState::Quarantined ? HealthState::Degraded : HealthState::Unknown;
            add_finding(FindingCode::AssetServiceWithheld, FindingSeverity::Warning, impact,
                        FindingDisposition::Supported,
                        observed + (state == LifecycleState::Quarantined
                                        ? "; the asset is withheld from service pending an integrity decision"
                                        : "; the asset is withdrawn from service for maintenance"));
            contribute(impact);
            break;
        }
        case ServiceClass::Draining:
            add_finding(FindingCode::AssetDraining, FindingSeverity::Warning, HealthState::Unknown,
                        FindingDisposition::Supported, observed + "; the asset is being drained");
            contribute(HealthState::Unknown);
            break;
        case ServiceClass::Decommissioned:
        case ServiceClass::Disposed:
            lifecycle_end_of_life_ = true;
            add_finding(FindingCode::AssetEndOfLife, FindingSeverity::Info, HealthState::Retired,
                        FindingDisposition::Supported,
                        observed + "; service health is not applicable to a decommissioned asset");
            add_finding(FindingCode::LifecycleObserved, FindingSeverity::Info, HealthState::Retired,
                        FindingDisposition::Informational, observed);
            break;
        case ServiceClass::PreService:
            add_finding(FindingCode::AssetNotYetInService, FindingSeverity::Warning, HealthState::Unknown,
                        FindingDisposition::Supported,
                        observed + "; the asset is not in service, so a service-health verdict is not available");
            contribute(HealthState::Unknown);
            break;
    }
    if (!last_finding().evidence.empty()) {
        // The finding above already carries the identifier; nothing further.
    }
}

void Evaluation::assess_maintenance() {
    std::vector<const Candidate*> statements;
    for (const Candidate& candidate : current_) {
        if (candidate.record.kind() == EvidenceKind::MaintenanceStatement) {
            statements.push_back(&candidate);
        }
    }
    if (statements.empty()) {
        add_finding(FindingCode::MaintenanceNotObserved, FindingSeverity::Info, HealthState::Healthy,
                    FindingDisposition::Informational,
                    "no maintenance statement has been admitted for this asset");
        return;
    }
    const Candidate* best_mask = nullptr;
    for (const Candidate* candidate : statements) {
        note_record(candidate->record);
        if (!candidate->freshness.usable) {
            add_finding(FindingCode::StaticEvidenceStale, FindingSeverity::Warning, HealthState::Healthy,
                        FindingDisposition::Uncertain,
                        "a maintenance statement observed at " +
                            candidate->record.observed_at().to_millisecond_string() +
                            " is outside the static freshness window and is not used");
            last_finding().evidence.push_back(candidate->record.id());
            continue;
        }
        const MaintenanceObservation& window = candidate->record.payload().maintenance;
        const bool covers_now = window.window_start <= context_.now && context_.now < window.window_end;
        const std::string description = std::string(to_string(window.kind)) + " " +
                                        window.window_start.to_millisecond_string() + ".." +
                                        window.window_end.to_millisecond_string();
        switch (window.state) {
            case MaintenanceState::Scheduled:
                add_finding(FindingCode::MaintenanceScheduledNotStarted, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational,
                            "maintenance window scheduled: " + description);
                break;
            case MaintenanceState::Active:
                if (!covers_now) {
                    add_finding(FindingCode::MaintenanceWindowEnded, FindingSeverity::Info, HealthState::Healthy,
                                FindingDisposition::Informational,
                                "the maintenance window is reported active but does not cover the evaluation "
                                "instant, so it masks nothing: " + description);
                    break;
                }
                add_finding(FindingCode::MaintenanceWindowActive, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational, "maintenance window active at the evaluation instant: " +
                                                                   description);
                if (window.mask != MaskScope::None && policy_.masking_allowed(window.kind)) {
                    if (best_mask == nullptr || EvidenceRecord::has_precedence_over(candidate->record, best_mask->record)) {
                        best_mask = candidate;
                    }
                } else if (window.mask != MaskScope::None) {
                    add_finding(FindingCode::MaskingRefused, FindingSeverity::Info, HealthState::Healthy,
                                FindingDisposition::Informational,
                                "the window asks for masking of " + std::string(to_string(window.mask)) +
                                    " but the policy does not allow masking for " +
                                    std::string(to_string(window.kind)) + " maintenance");
                }
                break;
            case MaintenanceState::Completed:
                add_finding(FindingCode::MaintenanceCompletedInWindow, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational, "maintenance completed: " + description);
                break;
            case MaintenanceState::Cancelled:
                add_finding(FindingCode::MaintenanceWindowEnded, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational, "maintenance window cancelled: " + description);
                break;
        }
        if (window.kind == MaintenanceKind::Corrective) {
            ++corrective_maintenance_count_;
        }
    }
    if (best_mask != nullptr) {
        mask_ = best_mask->record.payload().maintenance;
        mask_description_ = std::string(to_string(mask_->kind)) + " " + mask_->window_start.to_millisecond_string() +
                            ".." + mask_->window_end.to_millisecond_string();
        set_flag(AssessmentFlag::MaskedByMaintenance);
        add_finding(FindingCode::MaskingApplied, FindingSeverity::Info, HealthState::Healthy,
                    FindingDisposition::Informational,
                    "the active " + std::string(to_string(mask_->kind)) +
                        " window asks this observatory to set aside " + std::string(to_string(mask_->mask)) +
                        " findings while it is open, and the policy allows it");
        last_finding().evidence.push_back(best_mask->record.id());
    }
    if (corrective_maintenance_count_ >= 2) {
        add_finding(FindingCode::CorrectiveMaintenanceHistory, FindingSeverity::Warning, HealthState::Healthy,
                    FindingDisposition::Informational,
                    std::to_string(corrective_maintenance_count_) +
                        " corrective maintenance observations are on record for this asset");
    }
}

void Evaluation::assess_faults() {
    std::map<std::string, std::vector<const Candidate*>> groups;
    std::vector<std::string> order;
    for (const Candidate& candidate : current_) {
        if (candidate.record.kind() != EvidenceKind::FaultStatement) {
            continue;
        }
        const std::string key = candidate.record.fact_key();
        if (groups.find(key) == groups.end()) {
            order.push_back(key);
        }
        groups[key].push_back(&candidate);
    }
    std::sort(order.begin(), order.end());
    const Duration window = policy_.degradation().window;
    for (const std::string& key : order) {
        std::vector<const Candidate*>& group = groups[key];
        std::sort(group.begin(), group.end(), [](const Candidate* lhs, const Candidate* rhs) {
            return EvidenceRecord::has_precedence_over(lhs->record, rhs->record);
        });
        ++distinct_fault_keys_;
        bool recent = false;
        for (const Candidate* candidate : group) {
            if (context_.now.minus(candidate->record.observed_at()) <= window) {
                recent = true;
            }
        }
        if (recent) {
            ++recent_fault_keys_;
        }
        const Candidate* winner = group.front();
        note_record(winner->record);
        for (std::size_t index = 1; index < group.size(); ++index) {
            const Candidate* loser = group[index];
            note_record(loser->record);
            if (index > 3) {
                continue;
            }
            const FaultObservation& winner_fault = winner->record.payload().fault;
            const FaultObservation& loser_fault = loser->record.payload().fault;
            const bool same_source = winner->record.source() == loser->record.source();
            if (winner_fault.status == loser_fault.status && winner_fault.severity == loser_fault.severity) {
                add_finding(FindingCode::FaultDuplicateSuppressed, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational,
                            "a repeated statement of the same fault state was collapsed into the newest one: " +
                                std::string(to_string(winner_fault.severity)) + " " + winner_fault.code.str() + " on " +
                                winner_fault.component.str());
                last_finding().component = winner_fault.component;
                last_finding().evidence.push_back(winner->record.id());
                last_finding().evidence.push_back(loser->record.id());
            } else if (!same_source &&
                       class_rank(winner->record.source_class()) == class_rank(loser->record.source_class())) {
                set_flag(AssessmentFlag::ConflictingEvidence);
                add_finding(FindingCode::FaultConflicting, FindingSeverity::Warning, HealthState::Unknown,
                            FindingDisposition::Uncertain,
                            "two sources of the same class disagree about fault " + winner_fault.code.str() +
                                " on " + winner_fault.component.str() + ": " +
                                std::string(to_string(winner_fault.status)) + "/" +
                                std::string(to_string(winner_fault.severity)) + " and " +
                                std::string(to_string(loser_fault.status)) + "/" +
                                std::string(to_string(loser_fault.severity)));
                last_finding().component = winner_fault.component;
                last_finding().evidence.push_back(winner->record.id());
                last_finding().evidence.push_back(loser->record.id());
                contribute(HealthState::Unknown);
            } else {
                add_finding(FindingCode::FaultOutOfOrderSuperseded, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Superseded,
                            "an older statement of fault " + loser_fault.code.str() + " on " +
                                loser_fault.component.str() + " is superseded by the statement from " +
                                winner->record.source().str());
                last_finding().component = loser_fault.component;
                last_finding().evidence.push_back(loser->record.id());
                last_finding().evidence.push_back(winner->record.id());
            }
        }

        const FaultObservation& fault = winner->record.payload().fault;
        const bool masked = mask_.has_value() && masks_faults(mask_->mask);
        if (fault.status == FaultStatus::Cleared) {
            add_finding(FindingCode::FaultCleared, FindingSeverity::Info, HealthState::Healthy,
                        FindingDisposition::Informational,
                        "fault " + fault.code.str() + " on " + fault.component.str() + " is reported cleared");
            last_finding().component = fault.component;
            last_finding().evidence.push_back(winner->record.id());
            continue;
        }
        const HealthState impact = policy_.fault_impact(fault.severity);
        const std::string base = "fault " + fault.code.str() + " on " + fault.component.str() + " reported active at " +
                                 std::string(to_string(fault.severity)) + " severity by " +
                                 winner->record.source().str();
        if (!winner->freshness.usable) {
            const FindingCode code = winner->freshness.state == Freshness::Recovered ? FindingCode::FaultRecovered
                                                                                    : FindingCode::FaultStale;
            set_flag(AssessmentFlag::StaleEvidence);
            add_finding(code, FindingSeverity::Major, impact, FindingDisposition::Uncertain,
                        base + "; the statement is " + std::string(to_string(winner->freshness.state)) +
                            " and so cannot establish that the fault still stands");
            last_finding().component = fault.component;
            last_finding().evidence.push_back(winner->record.id());
            contribute(HealthState::Unknown);
            continue;
        }
        const int rank = static_cast<int>(fault.severity) + 1;
        max_active_fault_rank_ = std::max(max_active_fault_rank_, static_cast<std::int64_t>(rank));
        if (masked) {
            add_finding(FindingCode::FaultActive, FindingSeverity::Major, impact, FindingDisposition::Masked,
                        base + "; set aside while " + mask_description_ + " is open");
            last_finding().component = fault.component;
            last_finding().evidence.push_back(winner->record.id());
            last_finding().masked_by = mask_description_;
            continue;
        }
        add_finding(FindingCode::FaultActive, FindingSeverity::Major, impact, FindingDisposition::Supported, base);
        last_finding().component = fault.component;
        last_finding().evidence.push_back(winner->record.id());
        contribute(impact);
    }
    if (recent_fault_keys_ >= 2) {
        add_finding(FindingCode::FaultRecurrence, FindingSeverity::Warning, HealthState::Healthy,
                    FindingDisposition::Informational,
                    std::to_string(recent_fault_keys_) + " distinct faults were observed inside the last " +
                        policy_.degradation().window.to_seconds_string());
    }
}

void Evaluation::assess_telemetry() {
    std::map<MetricKind, std::vector<const Candidate*>> groups;
    for (const Candidate& candidate : current_) {
        if (candidate.record.kind() != EvidenceKind::TelemetryReading) {
            continue;
        }
        groups[candidate.record.payload().telemetry.metric].push_back(&candidate);
    }
    // Every metric the policy requires is accounted for, including the ones no
    // statement mentions at all. Reading only the metrics that appear in the
    // evidence would let an asset with no telemetry be reported without saying
    // that anything was missing.
    if (policy_.metric(MetricKind::Temperature).required) {
        // The scan below covers every metric; this branch exists so the loop is
        // not optimised into "only the metrics we happened to see".
    }
    for (std::size_t index = 0; index < kMetricKindCount; ++index) {
        const auto metric = static_cast<MetricKind>(index);
        if (!policy_.metric(metric).required || groups.find(metric) != groups.end()) {
            continue;
        }
        set_flag(AssessmentFlag::MissingRequiredEvidence);
        add_finding(FindingCode::MetricMissing, FindingSeverity::Warning, policy_.missing_required_cap(),
                    FindingDisposition::Supported,
                    "required metric " + std::string(asset_health::to_string(metric)) +
                        " has no reading at all, so nothing observed supports a healthy verdict");
        last_finding().metric = metric;
        contribute(policy_.missing_required_cap());
    }
    for (auto& entry : groups) {
        const MetricKind metric = entry.first;
        std::vector<const Candidate*>& group = entry.second;
        const MetricPolicy& metric_policy = policy_.metric(metric);
        std::vector<const Candidate*> usable;
        for (const Candidate* candidate : group) {
            note_record(candidate->record);
            if (candidate->freshness.usable) {
                usable.push_back(candidate);
            } else {
                const TelemetrySample& sample = candidate->record.payload().telemetry;
                FindingCode code = FindingCode::MetricStale;
                switch (candidate->freshness.state) {
                    case Freshness::Recovered:
                        code = FindingCode::MetricRecovered;
                        break;
                    case Freshness::SupersededEpoch:
                        code = FindingCode::MetricSourceEpochSuperseded;
                        break;
                    case Freshness::FutureDated:
                        code = FindingCode::MetricFutureDated;
                        break;
                    default:
                        code = FindingCode::MetricStale;
                        break;
                }
                if (code == FindingCode::MetricRecovered || code == FindingCode::MetricStale) {
                    set_flag(AssessmentFlag::StaleEvidence);
                }
                add_finding(code, FindingSeverity::Warning, policy_.missing_required_cap(),
                            FindingDisposition::Uncertain,
                            std::string(to_string(metric)) + " reading of " + sample.value.to_string() + " from " +
                                candidate->record.source().str() + " is " +
                                std::string(to_string(candidate->freshness.state)) + " and is not used: " +
                                candidate->freshness.explanation);
                last_finding().metric = metric;
                last_finding().evidence.push_back(candidate->record.id());
            }
        }
        if (usable.empty()) {
            if (metric_policy.required) {
                set_flag(AssessmentFlag::MissingRequiredEvidence);
                add_finding(FindingCode::MetricMissing, FindingSeverity::Warning, policy_.missing_required_cap(),
                            FindingDisposition::Supported,
                            "required metric " + std::string(to_string(metric)) +
                                " has no usable reading, so nothing observed supports a healthy verdict");
                last_finding().metric = metric;
                contribute(policy_.missing_required_cap());
            } else {
                add_finding(FindingCode::MetricMissing, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational,
                            "no usable reading of " + std::string(to_string(metric)) + ", which this policy does not require");
                last_finding().metric = metric;
            }
            continue;
        }

        std::vector<const Candidate*> good;
        std::vector<const Candidate*> limited;
        for (const Candidate* candidate : usable) {
            const SampleQuality quality = candidate->record.payload().telemetry.quality;
            if (quality == SampleQuality::Good) {
                good.push_back(candidate);
            } else {
                limited.push_back(candidate);
            }
        }
        // Only Good readings decide the verdict; the others are reported and
        // bound how healthy the answer may be.
        const std::vector<const Candidate*>& deciding = good.empty() ? usable : good;
        const EvidenceRecord* winner = winner_of(deciding);
        if (winner == nullptr) {
            continue;
        }
        const TelemetrySample& sample = winner->payload().telemetry;

        // Conflict detection among equally ranked sources.
        const int winner_rank = class_rank(winner->source_class());
        std::vector<const Candidate*> peers;
        for (const Candidate* candidate : deciding) {
            if (class_rank(candidate->record.source_class()) == winner_rank) {
                peers.push_back(candidate);
            }
        }
        bool conflicted = false;
        for (const Candidate* candidate : peers) {
            if (candidate->record.id() == winner->id()) {
                continue;
            }
            if (candidate->record.source() == winner->source()) {
                // One instrument reporting twice is a series of readings, not a
                // disagreement. Only a second source can contradict the first.
                continue;
            }
            const TelemetrySample& other = candidate->record.payload().telemetry;
            const std::int64_t difference = sample.value.absolute_difference_milli(other.value);
            if (difference <= conflict_tolerance(metric_policy, sample.value.milli(), other.value.milli())) {
                continue;
            }
            conflicted = true;
            set_flag(AssessmentFlag::ConflictingEvidence);
            const Excursion excursion = classify_excursion(metric_policy, other.value.milli());
            const HealthState impact = worse_of(policy_.conflict_impact(),
                                                excursion == Excursion::Critical   ? policy_.critical_impact()
                                                : excursion == Excursion::Warning ? policy_.warning_impact()
                                                                                  : HealthState::Healthy);
            add_finding(FindingCode::MetricConflicting, FindingSeverity::Major, impact,
                        FindingDisposition::Uncertain,
                        std::string(to_string(metric)) + " readings from sources of the same class differ by " +
                            std::to_string(difference) + " thousandths, beyond the tolerance of " +
                            std::to_string(conflict_tolerance(metric_policy, sample.value.milli(), other.value.milli())) +
                            ": " + sample.value.to_string() + " from " + winner->source().str() + " and " +
                            other.value.to_string() + " from " + candidate->record.source().str() +
                            "; both readings are preserved and neither is preferred");
            last_finding().metric = metric;
            last_finding().evidence.push_back(winner->id());
            last_finding().evidence.push_back(candidate->record.id());
            contribute(impact);
            break;
        }

        const Excursion excursion = classify_excursion(metric_policy, sample.value.milli());
        const bool masked = mask_.has_value() && masks_telemetry(mask_->mask);
        const std::string observed = std::string(to_string(metric)) + " " + sample.value.to_string() + " from " +
                                     winner->source().str() + " at " +
                                     winner->observed_at().to_millisecond_string();
        if (excursion != Excursion::None) {
            ++excursion_count_;
        }
        if (excursion == Excursion::Critical) {
            add_finding(FindingCode::MetricCriticalThreshold, FindingSeverity::Critical, policy_.critical_impact(),
                        masked ? FindingDisposition::Masked : FindingDisposition::Supported,
                        observed + " is beyond the critical bound");
            last_finding().metric = metric;
            last_finding().evidence.push_back(winner->id());
            if (masked) {
                last_finding().masked_by = mask_description_;
            } else {
                contribute(policy_.critical_impact());
            }
        } else if (excursion == Excursion::Warning) {
            add_finding(FindingCode::MetricWarningThreshold, FindingSeverity::Warning, policy_.warning_impact(),
                        masked ? FindingDisposition::Masked : FindingDisposition::Supported,
                        observed + " is beyond the warning bound");
            last_finding().metric = metric;
            last_finding().evidence.push_back(winner->id());
            if (masked) {
                last_finding().masked_by = mask_description_;
            } else {
                contribute(policy_.warning_impact());
            }
        } else {
            add_finding(FindingCode::MetricWithinThreshold, FindingSeverity::Info, HealthState::Healthy,
                        FindingDisposition::Informational, observed + " is inside every configured bound");
            last_finding().metric = metric;
            last_finding().evidence.push_back(winner->id());
        }

        // Quality limits how good the answer may be, but never hides an
        // excursion, which was already contributed above.
        const SampleQuality quality = sample.quality;
        HealthState quality_impact = HealthState::Healthy;
        if (quality == SampleQuality::Uncertain) {
            quality_impact = policy_.uncertain_reading_impact();
            add_finding(FindingCode::MetricUncertain, FindingSeverity::Warning, quality_impact,
                        FindingDisposition::Uncertain,
                        observed + " is flagged uncertain by its producer, so it cannot support a healthy verdict");
        } else if (quality == SampleQuality::Substituted) {
            quality_impact = policy_.uncertain_reading_impact();
            add_finding(FindingCode::MetricSubstituted, FindingSeverity::Warning, quality_impact,
                        FindingDisposition::Uncertain,
                        observed + " was substituted by its producer rather than read from the instrument");
        } else if (quality == SampleQuality::Bad) {
            quality_impact = policy_.missing_required_cap();
            add_finding(FindingCode::MetricBadQuality, FindingSeverity::Warning, quality_impact,
                        FindingDisposition::Uncertain,
                        observed + " is flagged bad by its producer and is not used to decide the verdict");
        }
        if (quality_impact != HealthState::Healthy) {
            last_finding().metric = metric;
            last_finding().evidence.push_back(winner->id());
            contribute(quality_impact);
        }
        if (sample.cached) {
            add_finding(FindingCode::MetricCached, FindingSeverity::Warning, policy_.cached_reading_impact(),
                        FindingDisposition::Uncertain,
                        observed + " was served from the producer's cache rather than read now");
            last_finding().metric = metric;
            last_finding().evidence.push_back(winner->id());
            contribute(policy_.cached_reading_impact());
        }
        for (const Candidate* candidate : limited) {
            if (candidate->record.id() == winner->id()) {
                continue;
            }
            note(candidate->record.id());
        }
        if (conflicted) {
            ++maintained_metric_count_;
        }
    }
}

void Evaluation::assess_firmware() {
    std::map<std::string, std::vector<const Candidate*>> groups;
    std::vector<std::string> order;
    for (const Candidate& candidate : current_) {
        if (candidate.record.kind() != EvidenceKind::FirmwareStatement) {
            continue;
        }
        const std::string key = candidate.record.fact_key();
        if (groups.find(key) == groups.end()) {
            order.push_back(key);
        }
        groups[key].push_back(&candidate);
    }
    if (order.empty()) {
        const bool in_service = !lifecycle_usable_ || lifecycle_end_of_life_ == false;
        if (!lifecycle_end_of_life_ && in_service) {
            set_flag(AssessmentFlag::UnknownFirmware);
        }
        add_finding(FindingCode::FirmwareNotObserved, FindingSeverity::Info, HealthState::Healthy,
                    FindingDisposition::Informational,
                    "no firmware statement has been admitted for this asset");
        return;
    }
    std::sort(order.begin(), order.end());
    for (const std::string& key : order) {
        std::vector<const Candidate*>& group = groups[key];
        std::sort(group.begin(), group.end(), [](const Candidate* lhs, const Candidate* rhs) {
            return EvidenceRecord::has_precedence_over(lhs->record, rhs->record);
        });
        const Candidate* winner = group.front();
        for (const Candidate* candidate : group) {
            note_record(candidate->record);
        }
        const FirmwareObservation& firmware = winner->record.payload().firmware;
        const std::string component = firmware.component.empty() ? std::string("the asset") : firmware.component.str();
        const std::string base = "firmware " + firmware.version.str() + " on " + component + " reported by " +
                                 winner->record.source().str();
        if (!winner->freshness.usable) {
            set_flag(AssessmentFlag::UnknownFirmware);
            add_finding(FindingCode::FirmwareStale, FindingSeverity::Warning, HealthState::Unknown,
                        FindingDisposition::Uncertain,
                        base + "; the statement is " + std::string(asset_health::to_string(winner->freshness.state)) +
                            " and no longer current");
            last_finding().evidence.push_back(winner->record.id());
            contribute(HealthState::Unknown);
            continue;
        }
        switch (firmware.compliance) {
            case FirmwareCompliance::Matches:
                firmware_known_ = true;
                firmware_compliant_ = true;
                add_finding(FindingCode::FirmwareMatchesBaseline, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational,
                            base + " matches the published baseline " +
                                (firmware.baseline.has_value() ? firmware.baseline->str() : std::string("(named)")));
                break;
            case FirmwareCompliance::Behind:
                firmware_known_ = true;
                add_finding(FindingCode::FirmwareBehindBaseline, FindingSeverity::Warning,
                            policy_.firmware_behind_impact(), FindingDisposition::Supported,
                            base + " is behind the published baseline " +
                                (firmware.baseline.has_value() ? firmware.baseline->str() : std::string("(named)")));
                contribute(policy_.firmware_behind_impact());
                break;
            case FirmwareCompliance::Ahead:
                firmware_known_ = true;
                add_finding(FindingCode::FirmwareAheadOfBaseline, FindingSeverity::Info, HealthState::Healthy,
                            FindingDisposition::Informational,
                            base + " is ahead of the published baseline " +
                                (firmware.baseline.has_value() ? firmware.baseline->str() : std::string("(named)")));
                break;
            case FirmwareCompliance::Unknown:
                set_flag(AssessmentFlag::UnknownFirmware);
                add_finding(FindingCode::FirmwareBaselineUnknown, FindingSeverity::Warning, HealthState::Healthy,
                            FindingDisposition::Informational,
                            base + "; the firmware authority has not published a comparison, so the baseline "
                                   "relationship is unknown and is counted as an unknown risk input");
                break;
        }
        last_finding().evidence.push_back(winner->record.id());
    }
}

void Evaluation::assess_degradation() {
    const DegradationPolicy& degradation = policy_.degradation();
    std::map<MetricKind, std::vector<const Candidate*>> groups;
    for (const Candidate& candidate : current_) {
        if (candidate.record.kind() != EvidenceKind::TelemetryReading || !candidate.freshness.usable) {
            continue;
        }
        if (candidate.record.payload().telemetry.quality != SampleQuality::Good) {
            continue;
        }
        if (context_.now.minus(candidate.record.observed_at()) > degradation.window) {
            continue;
        }
        groups[candidate.record.payload().telemetry.metric].push_back(&candidate);
    }
    for (auto& entry : groups) {
        std::vector<const Candidate*>& group = entry.second;
        if (group.size() < degradation.min_samples) {
            continue;
        }
        std::sort(group.begin(), group.end(), [](const Candidate* lhs, const Candidate* rhs) {
            if (lhs->record.observed_at() != rhs->record.observed_at()) {
                return lhs->record.observed_at() < rhs->record.observed_at();
            }
            return lhs->record.id() < rhs->record.id();
        });
        if (group.size() > degradation.max_samples) {
            group.erase(group.begin(), group.end() - static_cast<std::ptrdiff_t>(degradation.max_samples));
        }
        const MetricKind metric = entry.first;
        const std::int64_t first_value = group.front()->record.payload().telemetry.value.milli();
        const std::int64_t last_value = group.back()->record.payload().telemetry.value.milli();
        std::int64_t adverse = 0;
        std::string direction;
        switch (adverse_direction(metric)) {
            case AdverseDirection::Higher:
                adverse = last_value - first_value;
                direction = "higher is worse";
                break;
            case AdverseDirection::Lower:
                adverse = first_value - last_value;
                direction = "lower is worse";
                break;
            case AdverseDirection::TwoSided: {
                const MetricPolicy& metric_policy = policy_.metric(metric);
                const std::int64_t midpoint =
                    metric_policy.warn_low.has_value() && metric_policy.warn_high.has_value()
                        ? (*metric_policy.warn_low + *metric_policy.warn_high) / 2
                        : first_value;
                const std::int64_t first_distance = first_value >= midpoint ? first_value - midpoint : midpoint - first_value;
                const std::int64_t last_distance = last_value >= midpoint ? last_value - midpoint : midpoint - last_value;
                adverse = last_distance - first_distance;
                direction = "distance from the middle of the warning band";
                break;
            }
        }
        if (metric_semantics(metric) == MetricSemantics::Counter && last_value < first_value) {
            DegradationIndicator indicator;
            indicator.metric = metric;
            indicator.trend = TrendKind::Unknown;
            indicator.adverse_delta_milli = 0;
            indicator.sample_count = group.size();
            indicator.first_observed = group.front()->record.observed_at();
            indicator.last_observed = group.back()->record.observed_at();
            for (const Candidate* candidate : group) {
                indicator.evidence.push_back(candidate->record.id());
                note(candidate->record.id());
            }
            indicator.explanation = "the counter fell from " + std::to_string(first_value) + " to " +
                                    std::to_string(last_value) +
                                    " thousandths, which is a reset rather than a trend; no trend is reported";
            add_finding(FindingCode::DegradationInsufficientSamples, FindingSeverity::Info, HealthState::Healthy,
                        FindingDisposition::Informational, indicator.explanation);
            assessment_.degradation.push_back(std::move(indicator));
            continue;
        }
        DegradationIndicator indicator;
        indicator.metric = metric;
        indicator.adverse_delta_milli = adverse;
        indicator.sample_count = group.size();
        indicator.first_observed = group.front()->record.observed_at();
        indicator.last_observed = group.back()->record.observed_at();
        for (const Candidate* candidate : group) {
            indicator.evidence.push_back(candidate->record.id());
            note(candidate->record.id());
        }
        if (adverse > degradation.min_delta_milli) {
            indicator.trend = TrendKind::Worsening;
            ++worsening_count_;
            indicator.explanation = std::to_string(group.size()) + " readings over " +
                                    context_.now.minus(indicator.first_observed).to_seconds_string() +
                                    " moved " + std::to_string(adverse) + " thousandths in the adverse direction (" +
                                    direction + "), beyond the " +
                                    std::to_string(degradation.min_delta_milli) + " thousandth threshold";
            set_flag(AssessmentFlag::DegradationObserved);
            add_finding(FindingCode::DegradationWorsening, FindingSeverity::Warning, policy_.degradation_impact(),
                        FindingDisposition::Supported, indicator.explanation);
            last_finding().metric = metric;
            last_finding().evidence = indicator.evidence;
            contribute(policy_.degradation_impact());
        } else if (adverse < -degradation.min_delta_milli) {
            indicator.trend = TrendKind::Improving;
            indicator.explanation = std::to_string(group.size()) + " readings moved " + std::to_string(-adverse) +
                                    " thousandths against the adverse direction (" + direction + ")";
            add_finding(FindingCode::DegradationImproving, FindingSeverity::Info, HealthState::Healthy,
                        FindingDisposition::Informational, indicator.explanation);
            last_finding().metric = metric;
        } else {
            indicator.trend = TrendKind::Stable;
            indicator.explanation = std::to_string(group.size()) + " readings changed by " +
                                    std::to_string(adverse) + " thousandths, inside the " +
                                    std::to_string(degradation.min_delta_milli) + " thousandth threshold";
            add_finding(FindingCode::DegradationStable, FindingSeverity::Info, HealthState::Healthy,
                        FindingDisposition::Informational, indicator.explanation);
            last_finding().metric = metric;
        }
        assessment_.degradation.push_back(std::move(indicator));
    }
}

void Evaluation::assess_source_loss() {
    struct PerSource {
        std::size_t statements = 0;
        std::size_t usable = 0;
    };
    std::map<SourceId, PerSource> per_source;
    for (const Candidate& candidate : current_) {
        PerSource& entry = per_source[candidate.record.source()];
        ++entry.statements;
        if (candidate.freshness.usable) {
            ++entry.usable;
        }
    }
    for (const auto& entry : per_source) {
        if (entry.second.usable != 0 || entry.second.statements == 0) {
            continue;
        }
        set_flag(AssessmentFlag::SourceLossObserved);
        add_finding(FindingCode::SourceLoss, FindingSeverity::Warning, HealthState::Unknown,
                    FindingDisposition::Uncertain,
                    "source " + entry.first.str() + " has " + std::to_string(entry.second.statements) +
                        " statements on record for this asset and none of them is fresh, so the observatory is "
                        "no longer hearing from it");
        contribute(HealthState::Unknown);
    }
}

void Evaluation::compute_risk() {
    const RiskPolicy& risk = policy_.risk();
    RiskView view;
    view.formula =
        "total = sum(weight_i * min(raw_i, saturation_i) / saturation_i) / sum(weight_i), over the inputs that "
        "were available; score_i = min(raw_i, saturation_i) / saturation_i";

    auto add_input = [&view](const std::string& name, bool available, std::int64_t raw_milli,
                             std::int64_t saturation_milli, const Rational& weight, const std::string& explanation) {
        RiskInput input;
        input.name = name;
        input.available = available;
        input.raw_milli = raw_milli;
        input.saturation_milli = saturation_milli;
        input.weight = weight;
        input.explanation = explanation;
        const std::int64_t bounded = std::min(raw_milli, saturation_milli);
        input.score = Rational::make(bounded, saturation_milli).value_or(Rational::from_integer(0));
        input.contribution = input.score.multiply(weight).value_or(Rational::from_integer(0));
        view.inputs.push_back(std::move(input));
    };

    // 1. Age since entering service.
    bool age_available = service_entry_.has_value();
    std::int64_t age_hours_milli = 0;
    if (age_available) {
        const Duration age = context_.now.minus(*service_entry_);
        age_hours_milli = age.is_negative() ? 0 : age.hours() * 1000;
    }
    add_input("age-hours", age_available, age_hours_milli, risk.age_saturation_hours * 1000, risk.weight_age,
              age_available ? "hours since the lifecycle statement that put this asset in service"
                            : "no lifecycle statement carried the instant the asset entered service");

    // 2. Worst active fault severity.
    const bool fault_available = max_active_fault_rank_ > 0;
    add_input("active-fault-severity", fault_available, max_active_fault_rank_ * 1000, 4000,
              risk.weight_fault_severity,
              fault_available ? "the most severe active fault, on a scale where info is 1 and critical is 5"
                              : "no active and fresh fault statement was admitted");

    // 3. Fault recurrence in the degradation window.
    add_input("fault-recurrence", distinct_fault_keys_ > 0, static_cast<std::int64_t>(recent_fault_keys_) * 1000,
              risk.recurrence_saturation * 1000, risk.weight_fault_recurrence,
              distinct_fault_keys_ > 0
                  ? "distinct faults observed inside the last " +
                        policy_.degradation().window.to_seconds_string()
                  : "no fault statement was admitted for this asset");

    // 4. Worsening degradation indicators.
    add_input("degradation-indicators", !assessment_.degradation.empty(),
              static_cast<std::int64_t>(worsening_count_) * 1000, risk.degradation_saturation * 1000,
              risk.weight_degradation,
              assessment_.degradation.empty()
                  ? "no metric had enough fresh readings inside the degradation window to form a trend"
                  : "metrics whose trend inside the degradation window is adverse");

    // 5. Threshold excursions.
    add_input("threshold-excursions", maintained_metric_count_ > 0 || excursion_count_ > 0,
              static_cast<std::int64_t>(excursion_count_) * 1000, risk.excursion_saturation * 1000,
              risk.weight_threshold_excursion,
              excursion_count_ > 0 ? "metrics outside a configured warning or critical bound"
                                   : "no usable reading was outside a configured bound");

    // 6. Corrective maintenance burden.
    add_input("corrective-maintenance", corrective_maintenance_count_ > 0,
              static_cast<std::int64_t>(corrective_maintenance_count_) * 1000, risk.maintenance_saturation * 1000,
              risk.weight_maintenance_burden,
              corrective_maintenance_count_ > 0 ? "corrective maintenance observations on record"
                                                : "no corrective maintenance observation was admitted");

    // 7. Firmware state unknown or behind baseline.
    const bool firmware_risk = !firmware_known_ || !firmware_compliant_;
    // Without a single admitted statement there is nothing for an input to
    // measure, so the firmware input is unavailable rather than zero.
    add_input("firmware-state", !current_.empty(), firmware_risk ? 1000 : 0, 1000, risk.weight_firmware_unknown,
              firmware_known_ ? (firmware_compliant_ ? "the observed firmware matches the published baseline"
                                                     : "the observed firmware is behind the published baseline")
                              : "no firmware comparison is known for this asset");

    Rational weight_sum = Rational::from_integer(0);
    Rational contribution_sum = Rational::from_integer(0);
    std::size_t unavailable = 0;
    bool complete = true;
    for (RiskInput& input : view.inputs) {
        if (!input.available) {
            ++unavailable;
            complete = false;
            continue;
        }
        weight_sum = weight_sum.add(input.weight).value_or(weight_sum);
        contribution_sum = contribution_sum.add(input.contribution).value_or(contribution_sum);
    }
    view.complete = complete;
    view.available_weight_sum = weight_sum;
    if (weight_sum.is_zero()) {
        view.total = Rational::from_integer(0);
        view.band = RiskBand::Undetermined;
        view.justification = "no risk input was available, so no band is claimed";
    } else {
        view.total = contribution_sum.divide(weight_sum).value_or(Rational::from_integer(0));
        if (view.total < risk.band_low) {
            view.band = RiskBand::Low;
        } else if (view.total < risk.band_moderate) {
            view.band = RiskBand::Moderate;
        } else if (view.total < risk.band_high) {
            view.band = RiskBand::High;
        } else {
            view.band = RiskBand::Severe;
        }
        view.justification = "total " + view.total.to_string() + " (" + view.total.to_decimal_string(4) +
                             ") against boundaries low<" + risk.band_low.to_string() + ", moderate<" +
                             risk.band_moderate.to_string() + ", high<" + risk.band_high.to_string() +
                             ", so the band is " + std::string(to_string(view.band));
    }
    assessment_.risk = std::move(view);

    add_finding(FindingCode::RiskModelApplied, FindingSeverity::Info, HealthState::Healthy,
                FindingDisposition::Informational,
                "replacement risk " + std::string(to_string(assessment_.risk.band)) + ": " +
                    assessment_.risk.justification);
    if (unavailable != 0) {
        std::string names;
        for (const RiskInput& input : assessment_.risk.inputs) {
            if (input.available) {
                continue;
            }
            if (!names.empty()) {
                names.append(", ");
            }
            names.append(input.name);
        }
        add_finding(FindingCode::RiskInputUnavailable, FindingSeverity::Info, HealthState::Healthy,
                    FindingDisposition::Informational,
                    std::to_string(unavailable) + " risk input(s) had no evidence: " + names);
        add_finding(FindingCode::RiskModelIncomplete, FindingSeverity::Info, HealthState::Healthy,
                    FindingDisposition::Informational,
                    "the risk total is computed over the inputs that were available, so it is a partial view "
                    "and is reported as incomplete");
    }
}

}  // namespace

HealthAssessment evaluate(const EvaluationRequest& request, const HealthPolicy& policy,
                          const EvaluationContext& context) {
    Evaluation evaluation(request, policy, context);
    return evaluation.run();
}

Outcome<HealthAssessment> evaluate_checked(const EvaluationRequest& request, const HealthPolicy& policy,
                                           const EvaluationContext& context) {
    if (request.asset.is_nil()) {
        return Error(ErrorCode::MalformedAssetReference, "an assessment needs an asset identity");
    }
    if (request.generation.is_zero()) {
        return Error(ErrorCode::UnknownGeneration, "an assessment needs an incarnation to assess");
    }
    return evaluate(request, policy, context);
}

}  // namespace asset_health
