// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/provenance.hpp"

#include <array>
#include <cstddef>

namespace asset_health {
namespace {

struct SourceKindName {
    SourceKind kind;
    std::string_view name;
};

constexpr std::array<SourceKindName, kSourceKindCount> kSourceKindNames{{
    {SourceKind::AssetRegistry, "asset-registry"},
    {SourceKind::HardwareLifecycle, "hardware-lifecycle"},
    {SourceKind::MaintenanceCoordinator, "maintenance-coordinator"},
    {SourceKind::FirmwareBaseline, "firmware-baseline"},
    {SourceKind::TelemetryFeed, "telemetry-feed"},
    {SourceKind::FaultFeed, "fault-feed"},
    {SourceKind::SyntheticPlant, "synthetic-plant"},
    {SourceKind::Derived, "derived"},
    {SourceKind::OperatorStatement, "operator-statement"},
}};

constexpr std::array<std::string_view, kAuthorityDomainCount> kAuthorityDomainNames{{
    "identity",
    "lifecycle",
    "maintenance",
    "firmware",
    "observation",
}};

constexpr std::array<std::string_view, kSourceClassCount> kSourceClassNames{{
    "authoritative",
    "peer",
    "synthetic",
}};

}  // namespace

std::string_view to_string(SourceKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kSourceKindNames.size() ? kSourceKindNames[index].name : std::string_view{"unknown"};
}

std::string_view to_string(AuthorityDomain domain) noexcept {
    const auto index = static_cast<std::size_t>(domain);
    return index < kAuthorityDomainNames.size() ? kAuthorityDomainNames[index] : std::string_view{"unknown"};
}

std::string_view to_string(SourceClass klass) noexcept {
    const auto index = static_cast<std::size_t>(klass);
    return index < kSourceClassNames.size() ? kSourceClassNames[index] : std::string_view{"unknown"};
}

std::optional<SourceKind> parse_source_kind(std::string_view text) noexcept {
    for (const auto& entry : kSourceKindNames) {
        if (entry.name == text) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

std::optional<AuthorityDomain> parse_authority_domain(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kAuthorityDomainNames.size(); ++index) {
        if (kAuthorityDomainNames[index] == text) {
            return static_cast<AuthorityDomain>(index);
        }
    }
    return std::nullopt;
}

std::optional<SourceClass> parse_source_class(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kSourceClassNames.size(); ++index) {
        if (kSourceClassNames[index] == text) {
            return static_cast<SourceClass>(index);
        }
    }
    return std::nullopt;
}

AuthorityDomain authority_domain_of(SourceKind kind) noexcept {
    switch (kind) {
        case SourceKind::AssetRegistry:
            return AuthorityDomain::Identity;
        case SourceKind::HardwareLifecycle:
            return AuthorityDomain::Lifecycle;
        case SourceKind::MaintenanceCoordinator:
            return AuthorityDomain::Maintenance;
        case SourceKind::FirmwareBaseline:
            return AuthorityDomain::Firmware;
        case SourceKind::TelemetryFeed:
        case SourceKind::FaultFeed:
        case SourceKind::SyntheticPlant:
        case SourceKind::OperatorStatement:
        case SourceKind::Derived:
            return AuthorityDomain::Observation;
    }
    return AuthorityDomain::Observation;
}

bool may_speak_for(SourceKind kind, AuthorityDomain domain) noexcept {
    return is_ingestible(kind) && authority_domain_of(kind) == domain;
}

bool is_ingestible(SourceKind kind) noexcept { return kind != SourceKind::Derived; }

std::optional<Provenance> Provenance::make(Parts parts) noexcept {
    if (parts.source.empty()) {
        return std::nullopt;
    }
    if (!is_ingestible(parts.kind)) {
        return std::nullopt;
    }
    if (parts.sequence.is_zero()) {
        // Sequence 1 is the first statement of an epoch. Zero means the producer
        // did not number the statement, and an unnumbered statement cannot be
        // placed in a stream that may be replayed or fenced.
        return std::nullopt;
    }
    // The two instants are carried as facts rather than judged here. How far a
    // producer's clock may run ahead of this observatory's is a policy decision:
    // the observatory refuses a statement dated beyond its skew tolerance, and
    // the evaluator refuses to use one dated ahead of the instant it is assessing
    // for. Rejecting the pair outright would make both of those rules unreachable
    // and would hide which of them applied.
    Provenance value;
    value.source_ = std::move(parts.source);
    value.kind_ = parts.kind;
    value.klass_ = parts.klass;
    value.epoch_ = parts.epoch;
    value.sequence_ = parts.sequence;
    value.observed_at_ = parts.observed_at;
    value.received_at_ = parts.received_at;
    value.producer_version_ = std::move(parts.producer_version);
    return value;
}

std::string Provenance::to_string() const {
    std::string text;
    text.append(source_.str());
    text.push_back('/');
    text.append(asset_health::to_string(kind_));
    text.push_back('/');
    text.append(asset_health::to_string(klass_));
    text.append(" epoch=");
    text.append(epoch_.to_string());
    text.append(" seq=");
    text.append(sequence_.to_string());
    text.append(" observed=");
    text.append(observed_at_.to_millisecond_string());
    return text;
}

bool Provenance::precedes(const Provenance& lhs, const Provenance& rhs) noexcept {
    if (lhs.observed_at_ != rhs.observed_at_) {
        return rhs.observed_at_ < lhs.observed_at_;
    }
    if (lhs.klass_ != rhs.klass_) {
        return lhs.klass_ < rhs.klass_;
    }
    if (lhs.epoch_ != rhs.epoch_) {
        return rhs.epoch_ < lhs.epoch_;
    }
    if (lhs.sequence_ != rhs.sequence_) {
        return rhs.sequence_ < lhs.sequence_;
    }
    return lhs.source_ < rhs.source_;
}

}  // namespace asset_health
