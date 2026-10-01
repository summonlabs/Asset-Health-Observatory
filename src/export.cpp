// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/export.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace asset_health {
namespace {

/// A minimal canonical JSON emitter. Indentation is fixed at two spaces and keys
/// are written in the order the code writes them, so two runs produce the same
/// bytes for the same answer.
class Json {
public:
    explicit Json(std::string& out) : out_(out) {}

    void begin_object() {
        out_.push_back('{');
        ++depth_;
        first_ = true;
    }
    void end_object() {
        --depth_;
        newline_if_needed();
        out_.push_back('}');
        first_ = false;
    }
    void begin_array() {
        out_.push_back('[');
        ++depth_;
        first_ = true;
    }
    void end_array() {
        --depth_;
        newline_if_needed();
        out_.push_back(']');
        first_ = false;
    }
    void key(const char* name) {
        separate();
        out_.push_back('"');
        out_.append(name);
        out_.append("\": ");
        first_ = true;
    }
    void string(const std::string& value) {
        separate();
        append_string(value);
        first_ = false;
    }
    void number(std::int64_t value) {
        separate();
        out_.append(std::to_string(value));
        first_ = false;
    }
    void number(std::uint64_t value) {
        separate();
        out_.append(std::to_string(value));
        first_ = false;
    }
    void number(double value) {
        separate();
        char buffer[64];
        const int written = std::snprintf(buffer, sizeof(buffer), "%.4f", value);
        out_.append(buffer, written > 0 ? static_cast<std::size_t>(written) : 0u);
        first_ = false;
    }
    void boolean(bool value) {
        separate();
        out_.append(value ? "true" : "false");
        first_ = false;
    }
    void null_value() {
        separate();
        out_.append("null");
        first_ = false;
    }

private:
    void separate() {
        if (first_) {
            first_ = false;
            return;
        }
        out_.push_back(',');
        newline();
    }
    void newline_if_needed() {
        if (!first_) {
            newline();
        }
    }
    void newline() {
        out_.push_back('\n');
        out_.append(static_cast<std::size_t>(depth_) * 2, ' ');
    }
    void append_string(const std::string& value) {
        out_.push_back('"');
        for (const char character : value) {
            const auto byte = static_cast<unsigned char>(character);
            switch (character) {
                case '"':
                    out_.append("\\\"");
                    break;
                case '\\':
                    out_.append("\\\\");
                    break;
                case '\n':
                    out_.append("\\n");
                    break;
                case '\r':
                    out_.append("\\r");
                    break;
                case '\t':
                    out_.append("\\t");
                    break;
                default:
                    if (byte < 0x20) {
                        char buffer[8];
                        const int written = std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                        out_.append(buffer, written > 0 ? static_cast<std::size_t>(written) : 0u);
                    } else {
                        out_.push_back(character);
                    }
                    break;
            }
        }
        out_.push_back('"');
    }

    std::string& out_;
    int depth_ = 0;
    bool first_ = true;
};

void write_freshness(Json& json, Freshness freshness) {
    json.begin_object();
    json.key("state");
    json.string(std::string(to_string(freshness)));
    json.key("usable");
    json.boolean(is_usable(freshness));
    json.end_object();
}

void write_finding(Json& json, const Finding& finding) {
    json.begin_object();
    json.key("code");
    json.string(std::string(to_string(finding.code)));
    json.key("severity");
    json.string(std::string(to_string(finding.severity)));
    json.key("impact");
    json.string(std::string(to_string(finding.impact)));
    json.key("disposition");
    json.string(std::string(to_string(finding.disposition)));
    json.key("detail");
    json.string(finding.detail);
    if (finding.metric.has_value()) {
        json.key("metric");
        json.string(std::string(to_string(*finding.metric)));
    }
    if (finding.component.has_value()) {
        json.key("component");
        json.string(finding.component->str());
    }
    if (finding.masked_by.has_value()) {
        json.key("masked_by");
        json.string(*finding.masked_by);
    }
    json.key("evidence");
    json.begin_array();
    for (const EvidenceId& id : finding.evidence) {
        json.string(id.to_string());
    }
    json.end_array();
    json.end_object();
}

void write_risk_input(Json& json, const RiskInput& input) {
    json.begin_object();
    json.key("name");
    json.string(input.name);
    json.key("available");
    json.boolean(input.available);
    json.key("raw_milli");
    json.number(input.raw_milli);
    json.key("saturation_milli");
    json.number(input.saturation_milli);
    json.key("score");
    json.string(input.score.to_string());
    json.key("weight");
    json.string(input.weight.to_string());
    json.key("contribution");
    json.string(input.contribution.to_string());
    json.key("explanation");
    json.string(input.explanation);
    json.key("evidence");
    json.begin_array();
    for (const EvidenceId& id : input.evidence) {
        json.string(id.to_string());
    }
    json.end_array();
    json.end_object();
}

}  // namespace

std::string to_json(const HealthAssessment& assessment) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("schema_version");
    json.number(static_cast<std::uint64_t>(limits::kAssessmentSchemaVersion));
    json.key("asset");
    json.string(assessment.asset.to_string());
    json.key("generation");
    json.number(static_cast<std::uint64_t>(assessment.generation.value()));
    json.key("identity_revision");
    json.number(static_cast<std::uint64_t>(assessment.identity_revision.value()));
    json.key("state");
    json.string(std::string(to_string(assessment.state)));
    json.key("state_description");
    json.string(health_state_description(assessment.state));
    json.key("flags");
    json.begin_array();
    for (const AssessmentFlag flag : all_assessment_flags()) {
        if (assessment.flags.contains(flag)) {
            json.string(std::string(to_string(flag)));
        }
    }
    json.end_array();
    json.key("evaluated_at");
    json.string(assessment.evaluated_at.to_string());
    json.key("policy_id");
    json.string(assessment.policy_id);
    json.key("policy_version");
    json.number(static_cast<std::uint64_t>(assessment.policy_version.value()));
    json.key("policy_fingerprint");
    json.string(assessment.policy_fingerprint);
    json.key("assessment_revision");
    json.number(static_cast<std::uint64_t>(assessment.revision.value()));
    json.key("evidence_considered");
    json.number(static_cast<std::uint64_t>(assessment.evidence_considered));
    json.key("evidence_depended_on");
    json.number(static_cast<std::uint64_t>(assessment.evidence_depended_on));
    json.key("findings");
    json.begin_array();
    for (const Finding& finding : assessment.findings) {
        write_finding(json, finding);
    }
    json.end_array();
    json.key("degradation");
    json.begin_array();
    for (const DegradationIndicator& indicator : assessment.degradation) {
        json.begin_object();
        json.key("metric");
        json.string(std::string(to_string(indicator.metric)));
        json.key("trend");
        json.string(std::string(to_string(indicator.trend)));
        json.key("adverse_delta_milli");
        json.number(indicator.adverse_delta_milli);
        json.key("samples");
        json.number(static_cast<std::uint64_t>(indicator.sample_count));
        json.key("first_observed");
        json.string(indicator.first_observed.to_string());
        json.key("last_observed");
        json.string(indicator.last_observed.to_string());
        json.key("explanation");
        json.string(indicator.explanation);
        json.key("evidence");
        json.begin_array();
        for (const EvidenceId& id : indicator.evidence) {
            json.string(id.to_string());
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.key("dependencies");
    json.begin_array();
    for (const EvidenceDependency& dependency : assessment.dependencies) {
        json.begin_object();
        json.key("evidence");
        json.string(dependency.id.to_string());
        json.key("kind");
        json.string(std::string(to_string(dependency.kind)));
        json.key("source");
        json.string(dependency.source.str());
        json.key("source_kind");
        json.string(std::string(to_string(dependency.source_kind)));
        json.key("source_class");
        json.string(std::string(to_string(dependency.source_class)));
        json.key("observed_at");
        json.string(dependency.observed_at.to_string());
        json.key("freshness");
        json.string(std::string(to_string(dependency.freshness)));
        json.key("fresh");
        json.boolean(dependency.fresh);
        json.key("synthetic");
        json.boolean(dependency.synthetic);
        json.end_object();
    }
    json.end_array();
    json.key("risk");
    json.begin_object();
    json.key("band");
    json.string(std::string(to_string(assessment.risk.band)));
    json.key("total");
    json.string(assessment.risk.total.to_string());
    json.key("total_decimal");
    json.string(assessment.risk.total.to_decimal_string(6));
    json.key("available_weight");
    json.string(assessment.risk.available_weight_sum.to_string());
    json.key("complete");
    json.boolean(assessment.risk.complete);
    json.key("formula");
    json.string(assessment.risk.formula);
    json.key("justification");
    json.string(assessment.risk.justification);
    json.key("inputs");
    json.begin_array();
    for (const RiskInput& input : assessment.risk.inputs) {
        write_risk_input(json, input);
    }
    json.end_array();
    json.end_object();
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const HistoryView& history) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("asset");
    json.string(history.asset.to_string());
    json.key("generation");
    json.number(static_cast<std::uint64_t>(history.generation.value()));
    json.key("total_published");
    json.number(static_cast<std::uint64_t>(history.total_published));
    json.key("truncated");
    json.boolean(history.truncated);
    json.key("entries");
    json.begin_array();
    for (const HistoryEntry& entry : history.entries) {
        json.begin_object();
        json.key("revision");
        json.number(static_cast<std::uint64_t>(entry.revision.value()));
        json.key("state");
        json.string(std::string(to_string(entry.state)));
        json.key("flags");
        json.string(entry.flags.to_string());
        json.key("risk_band");
        json.string(std::string(to_string(entry.risk_band)));
        json.key("evaluated_at");
        json.string(entry.evaluated_at.to_string());
        json.key("recorded_at");
        json.string(entry.recorded_at.to_string());
        json.key("policy_fingerprint");
        json.string(entry.policy_fingerprint);
        json.key("findings");
        json.number(static_cast<std::uint64_t>(entry.finding_count));
        json.key("attention_findings");
        json.number(static_cast<std::uint64_t>(entry.attention_findings));
        json.key("evidence_depended_on");
        json.number(static_cast<std::uint64_t>(entry.evidence_depended_on));
        json.end_object();
    }
    json.end_array();
    json.key("transitions");
    json.begin_array();
    for (const HealthTransition& transition : history.transitions) {
        json.begin_object();
        json.key("from_revision");
        json.number(static_cast<std::uint64_t>(transition.from_revision.value()));
        json.key("to_revision");
        json.number(static_cast<std::uint64_t>(transition.to_revision.value()));
        json.key("from");
        json.string(std::string(to_string(transition.from)));
        json.key("to");
        json.string(std::string(to_string(transition.to)));
        json.key("at");
        json.string(transition.at.to_string());
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const EvidenceRecord& record) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("evidence");
    json.string(record.id().to_string());
    json.key("asset");
    json.string(record.subject().asset().to_string());
    json.key("generation");
    json.number(static_cast<std::uint64_t>(record.subject().generation().value()));
    json.key("kind");
    json.string(std::string(to_string(record.kind())));
    json.key("fact");
    json.string(record.fact_key());
    json.key("source");
    json.string(record.source().str());
    json.key("source_kind");
    json.string(std::string(to_string(record.source_kind())));
    json.key("source_class");
    json.string(std::string(to_string(record.source_class())));
    json.key("stream_epoch");
    json.number(static_cast<std::uint64_t>(record.provenance().epoch().value()));
    json.key("stream_sequence");
    json.number(static_cast<std::uint64_t>(record.provenance().sequence().value()));
    json.key("observed_at");
    json.string(record.observed_at().to_string());
    json.key("received_at");
    json.string(record.received_at().to_string());
    json.key("commit_epoch");
    json.number(static_cast<std::uint64_t>(record.commit_epoch().value()));
    json.key("synthetic");
    json.boolean(record.is_synthetic());
    json.key("payload");
    json.begin_object();
    switch (record.kind()) {
        case EvidenceKind::IdentityStatement:
            json.key("identity_revision");
            json.number(static_cast<std::uint64_t>(record.payload().identity.revision.value()));
            json.key("current");
            json.boolean(record.payload().identity.current);
            if (record.payload().identity.serial.has_value()) {
                json.key("serial");
                json.string(record.payload().identity.serial->str());
            }
            break;
        case EvidenceKind::LifecycleStatement:
            json.key("lifecycle_state");
            json.string(std::string(to_string(record.payload().lifecycle.state)));
            break;
        case EvidenceKind::MaintenanceStatement:
            json.key("maintenance_kind");
            json.string(std::string(to_string(record.payload().maintenance.kind)));
            json.key("maintenance_state");
            json.string(std::string(to_string(record.payload().maintenance.state)));
            json.key("window_start");
            json.string(record.payload().maintenance.window_start.to_string());
            json.key("window_end");
            json.string(record.payload().maintenance.window_end.to_string());
            json.key("mask");
            json.string(std::string(to_string(record.payload().maintenance.mask)));
            break;
        case EvidenceKind::FirmwareStatement:
            json.key("component");
            json.string(record.payload().firmware.component.str());
            json.key("version");
            json.string(record.payload().firmware.version.str());
            json.key("compliance");
            json.string(std::string(to_string(record.payload().firmware.compliance)));
            if (record.payload().firmware.baseline.has_value()) {
                json.key("baseline");
                json.string(record.payload().firmware.baseline->str());
            }
            break;
        case EvidenceKind::TelemetryReading:
            json.key("metric");
            json.string(std::string(to_string(record.payload().telemetry.metric)));
            json.key("value");
            json.string(record.payload().telemetry.value.to_string());
            json.key("value_milli");
            json.number(record.payload().telemetry.value.milli());
            json.key("quality");
            json.string(std::string(to_string(record.payload().telemetry.quality)));
            json.key("cached");
            json.boolean(record.payload().telemetry.cached);
            break;
        case EvidenceKind::FaultStatement:
            json.key("component");
            json.string(record.payload().fault.component.str());
            json.key("fault_code");
            json.string(record.payload().fault.code.str());
            json.key("severity");
            json.string(std::string(to_string(record.payload().fault.severity)));
            json.key("status");
            json.string(std::string(to_string(record.payload().fault.status)));
            break;
    }
    json.end_object();
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const RefusalRecord& refusal) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("asset");
    if (refusal.asset.has_value()) {
        json.string(refusal.asset->to_string());
    } else {
        json.null_value();
    }
    json.key("generation");
    json.number(static_cast<std::uint64_t>(refusal.generation.value()));
    json.key("source");
    json.string(refusal.source.str());
    json.key("source_kind");
    json.string(std::string(to_string(refusal.source_kind)));
    json.key("claimed_kind");
    if (refusal.claimed_kind.has_value()) {
        json.string(std::string(to_string(*refusal.claimed_kind)));
    } else {
        json.null_value();
    }
    json.key("code");
    json.string(std::string(error_code_name(refusal.code)));
    json.key("detail");
    json.string(refusal.detail);
    json.key("received_at");
    json.string(refusal.received_at.to_string());
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const RecoveryReport& report) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("created");
    json.boolean(report.created);
    json.key("existing");
    json.boolean(report.existing);
    json.key("previous_close_clean");
    json.boolean(report.previous_close_clean);
    json.key("torn_tail_discarded");
    json.boolean(report.torn_tail_discarded);
    json.key("previous_epoch");
    json.number(static_cast<std::uint64_t>(report.previous_epoch.value()));
    json.key("current_epoch");
    json.number(static_cast<std::uint64_t>(report.current_epoch.value()));
    json.key("recovered_generation");
    json.number(static_cast<std::uint64_t>(report.recovered_generation.value()));
    json.key("evidence_records");
    json.number(report.evidence_records);
    json.key("history_records");
    json.number(report.history_records);
    json.key("dynamic_records_demoted");
    json.number(report.dynamic_records_demoted);
    json.key("uncommitted_discarded");
    json.begin_array();
    for (const GenerationSequence& generation : report.uncommitted_discarded) {
        json.number(static_cast<std::uint64_t>(generation.value()));
    }
    json.end_array();
    json.key("damaged_below_recovered");
    json.begin_array();
    for (const GenerationSequence& generation : report.damaged_below_recovered) {
        json.number(static_cast<std::uint64_t>(generation.value()));
    }
    json.end_array();
    json.key("detail");
    json.string(report.detail);
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const StoreAudit& audit) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("ok");
    json.boolean(audit.ok);
    json.key("found");
    json.boolean(audit.found);
    json.key("store_id");
    json.string(audit.store_id);
    json.key("format_version");
    json.number(static_cast<std::uint64_t>(audit.format_version.value()));
    json.key("current_generation");
    json.number(static_cast<std::uint64_t>(audit.current_generation.value()));
    json.key("epoch");
    json.number(static_cast<std::uint64_t>(audit.epoch.value()));
    json.key("evidence_records");
    json.number(audit.evidence_records);
    json.key("history_records");
    json.number(audit.history_records);
    json.key("retry_records");
    json.number(audit.retry_records);
    auto write_list = [&json](const char* name, const std::vector<GenerationSequence>& values) {
        json.key(name);
        json.begin_array();
        for (const GenerationSequence& generation : values) {
            json.number(static_cast<std::uint64_t>(generation.value()));
        }
        json.end_array();
    };
    write_list("generations", audit.generations);
    write_list("valid", audit.valid_generations);
    write_list("torn", audit.torn_generations);
    write_list("corrupt", audit.corrupt_generations);
    write_list("uncommitted", audit.uncommitted_generations);
    write_list("missing", audit.missing_generations);
    json.key("problems");
    json.begin_array();
    for (const std::string& problem : audit.problems) {
        json.string(problem);
    }
    json.end_array();
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const IngestOutcome& outcome) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("admitted");
    json.boolean(outcome.admitted);
    json.key("evidence");
    if (outcome.evidence_id.is_nil()) {
        json.null_value();
    } else {
        json.string(outcome.evidence_id.to_string());
    }
    json.key("generation");
    json.number(static_cast<std::uint64_t>(outcome.generation.value()));
    json.key("refusal_code");
    json.string(std::string(error_code_name(outcome.refusal_code)));
    json.key("detail");
    json.string(outcome.detail);
    json.key("duplicate_delivery");
    json.boolean(outcome.duplicate_delivery);
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const HealthPolicy& policy) {
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("id");
    json.string(policy.id().str());
    json.key("version");
    json.number(static_cast<std::uint64_t>(policy.version().value()));
    json.key("fingerprint");
    json.string(policy.fingerprint());
    json.key("canonical_text");
    json.string(policy.to_string());
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string to_json(const std::vector<EvidenceRecord>& records) {
    std::string out;
    Json json(out);
    json.begin_array();
    for (const EvidenceRecord& record : records) {
        json.begin_object();
        json.key("evidence");
        json.string(record.id().to_string());
        json.key("asset");
        json.string(record.subject().asset().to_string());
        json.key("generation");
        json.number(static_cast<std::uint64_t>(record.subject().generation().value()));
        json.key("kind");
        json.string(std::string(to_string(record.kind())));
        json.key("fact");
        json.string(record.fact_key());
        json.key("source");
        json.string(record.source().str());
        json.key("observed_at");
        json.string(record.observed_at().to_string());
        json.key("commit_epoch");
        json.number(static_cast<std::uint64_t>(record.commit_epoch().value()));
        json.end_object();
    }
    json.end_array();
    out.push_back('\n');
    return out;
}

std::string statistics_json(const StoreState& state) {
    std::set<AssetRefId> assets;
    std::map<EvidenceKind, std::size_t> per_kind;
    std::set<SourceId> sources;
    std::size_t synthetic = 0;
    for (const EvidenceRecord& record : state.evidence) {
        assets.insert(record.subject().asset());
        ++per_kind[record.kind()];
        sources.insert(record.source());
        if (record.is_synthetic()) {
            ++synthetic;
        }
    }
    std::string out;
    Json json(out);
    json.begin_object();
    json.key("epoch");
    json.number(static_cast<std::uint64_t>(state.epoch.value()));
    json.key("generation");
    json.number(static_cast<std::uint64_t>(state.generation.value()));
    json.key("pruned_below");
    json.number(static_cast<std::uint64_t>(state.pruned_below.value()));
    json.key("recovered");
    json.boolean(state.recovered);
    json.key("assets");
    json.number(static_cast<std::uint64_t>(assets.size()));
    json.key("sources");
    json.number(static_cast<std::uint64_t>(sources.size()));
    json.key("evidence");
    json.number(static_cast<std::uint64_t>(state.evidence.size()));
    json.key("evidence_synthetic");
    json.number(static_cast<std::uint64_t>(synthetic));
    json.key("history");
    json.number(static_cast<std::uint64_t>(state.history.size()));
    json.key("refusals");
    json.number(static_cast<std::uint64_t>(state.refusals.size()));
    json.key("retries");
    json.number(static_cast<std::uint64_t>(state.retries.size()));
    json.key("streams");
    json.number(static_cast<std::uint64_t>(state.source_streams.size()));
    json.key("evidence_by_kind");
    json.begin_object();
    for (const EvidenceKind kind : all_evidence_kinds()) {
        json.key(std::string(to_string(kind)).c_str());
        json.number(static_cast<std::uint64_t>(per_kind[kind]));
    }
    json.end_object();
    json.end_object();
    out.push_back('\n');
    return out;
}

std::string sources_json(const StoreState& state) {
    std::string out;
    Json json(out);
    json.begin_array();
    for (const SourceStreamState& stream : state.source_streams) {
        json.begin_object();
        json.key("source");
        json.string(stream.source.str());
        json.key("kind");
        json.string(std::string(to_string(stream.kind)));
        json.key("epoch");
        json.number(static_cast<std::uint64_t>(stream.epoch.value()));
        json.key("last_sequence");
        json.number(static_cast<std::uint64_t>(stream.last_sequence.value()));
        json.key("last_observed_at");
        json.string(stream.last_observed_at.to_string());
        json.key("last_received_at");
        json.string(stream.last_received_at.to_string());
        json.key("last_evidence");
        json.string(stream.last_evidence_id.to_string());
        json.end_object();
    }
    json.end_array();
    out.push_back('\n');
    return out;
}

}  // namespace asset_health
