// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/history.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace asset_health {

bool HistoryEntry::canonical_less(const HistoryEntry& lhs, const HistoryEntry& rhs) noexcept {
    if (lhs.asset != rhs.asset) {
        return lhs.asset < rhs.asset;
    }
    if (lhs.generation != rhs.generation) {
        return lhs.generation < rhs.generation;
    }
    return lhs.revision < rhs.revision;
}

std::string HistoryEntry::to_string() const {
    std::string text;
    text.append(asset.to_string());
    text.append("@g");
    text.append(generation.to_string());
    text.append(" r");
    text.append(revision.to_string());
    text.push_back(' ');
    text.append(asset_health::to_string(state));
    text.append(" risk ");
    text.append(asset_health::to_string(risk_band));
    text.append(" evaluated ");
    text.append(evaluated_at.to_millisecond_string());
    text.append(" recorded ");
    text.append(recorded_at.to_millisecond_string());
    text.append(" findings ");
    text.append(std::to_string(finding_count));
    text.append(" attention ");
    text.append(std::to_string(attention_findings));
    text.append(" flags ");
    const std::string flags_text = this->flags.to_string();
    text.append(flags_text.empty() ? std::string("none") : flags_text);
    text.append(" policy ");
    text.append(policy_fingerprint);
    return text;
}

std::string HistoryView::to_string() const {
    std::string text;
    text.append("history ");
    text.append(asset.to_string());
    text.append("@g");
    text.append(generation.to_string());
    text.append(" entries ");
    text.append(std::to_string(entries.size()));
    text.append(" published ");
    text.append(std::to_string(total_published));
    text.append(" truncated ");
    text.append(truncated ? "true" : "false");
    text.push_back('\n');
    for (const HistoryEntry& entry : entries) {
        text.append("entry ");
        text.append(entry.to_string());
        text.push_back('\n');
    }
    for (const HealthTransition& transition : transitions) {
        text.append("transition r");
        text.append(transition.from_revision.to_string());
        text.append("->r");
        text.append(transition.to_revision.to_string());
        text.push_back(' ');
        text.append(asset_health::to_string(transition.from));
        text.append(" -> ");
        text.append(asset_health::to_string(transition.to));
        text.append(" at ");
        text.append(transition.at.to_millisecond_string());
        text.push_back('\n');
    }
    return text;
}

HistoryEntry make_history_entry(const HealthAssessment& assessment, Instant recorded_at) {
    HistoryEntry entry;
    entry.revision = assessment.revision;
    entry.asset = assessment.asset;
    entry.generation = assessment.generation;
    entry.state = assessment.state;
    entry.flags = assessment.flags;
    entry.risk_band = assessment.risk.band;
    entry.evaluated_at = assessment.evaluated_at;
    entry.recorded_at = recorded_at;
    entry.policy_fingerprint = assessment.policy_fingerprint;
    entry.finding_count = static_cast<std::uint16_t>(
        std::min<std::size_t>(assessment.findings.size(), static_cast<std::size_t>(0xFFFFu)));
    std::size_t attention = 0;
    for (const Finding& finding : assessment.findings) {
        if (finding.disposition == FindingDisposition::Supported && finding.impact != HealthState::Healthy) {
            ++attention;
        }
    }
    entry.attention_findings = static_cast<std::uint16_t>(std::min<std::size_t>(attention, 0xFFFFu));
    entry.evidence_depended_on = static_cast<std::uint32_t>(
        std::min<std::size_t>(assessment.evidence_depended_on, static_cast<std::size_t>(0xFFFFFFFFu)));
    return entry;
}

HistoryView build_history(const std::vector<HistoryEntry>& entries, const AssetRefId& asset,
                          AssetGeneration generation) {
    HistoryView view;
    view.asset = asset;
    view.generation = generation;
    std::vector<HistoryEntry> matching;
    matching.reserve(entries.size());
    for (const HistoryEntry& entry : entries) {
        if (entry.asset == asset && entry.generation == generation) {
            matching.push_back(entry);
        }
    }
    std::sort(matching.begin(), matching.end(),
              [](const HistoryEntry& lhs, const HistoryEntry& rhs) { return HistoryEntry::canonical_less(lhs, rhs); });
    view.total_published = matching.size();
    for (auto iterator = matching.rbegin(); iterator != matching.rend(); ++iterator) {
        view.entries.push_back(*iterator);
    }
    for (std::size_t index = 1; index < matching.size(); ++index) {
        const HistoryEntry& previous = matching[index - 1];
        const HistoryEntry& current = matching[index];
        if (previous.state == current.state) {
            continue;
        }
        HealthTransition transition;
        transition.from_revision = previous.revision;
        transition.to_revision = current.revision;
        transition.from = previous.state;
        transition.to = current.state;
        transition.at = current.evaluated_at;
        view.transitions.push_back(transition);
    }
    return view;
}

}  // namespace asset_health
