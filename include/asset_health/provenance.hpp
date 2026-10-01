// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Where an observation came from, and what it is allowed to say.
//
// The authority domain is the boundary in code. A source declares which DCCP
// authority it speaks for; evidence of a kind owned by another domain is refused
// at admission with AuthorityDomainViolation. Seeing a lifecycle state in an
// evidence document therefore never makes this observatory the lifecycle
// authority: it makes it a reader of one statement about one asset.

#ifndef ASSET_HEALTH_PROVENANCE_HPP
#define ASSET_HEALTH_PROVENANCE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "asset_health/error.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// Which DCCP authority, or which class of instrument, produced an observation.
enum class SourceKind : std::uint8_t {
    /// The authoritative physical asset identity registry.
    AssetRegistry = 0,
    /// The authoritative hardware lifecycle authority.
    HardwareLifecycle = 1,
    /// The authoritative maintenance coordination authority.
    MaintenanceCoordinator = 2,
    /// The authoritative firmware baseline authority.
    FirmwareBaseline = 3,
    /// A telemetry producer: real instrumentation, or a model of it that says so.
    TelemetryFeed = 4,
    /// A fault or alarm producer.
    FaultFeed = 5,
    /// A modelled plant, sensor set or economic input. Never presented as measured.
    SyntheticPlant = 6,
    /// Produced by this observatory from admitted evidence. Never accepted at ingest.
    Derived = 7,
    /// A human operator statement.
    OperatorStatement = 8,
};

/// Number of SourceKind enumerators, so valid values are 0 .. count - 1.
inline constexpr std::size_t kSourceKindCount = 9;

/// The domain a source speaks for. Evidence is admitted only into the domain its
/// source owns, which is what keeps an observation from becoming a claim of
/// authority.
enum class AuthorityDomain : std::uint8_t {
    /// Identity and incarnation facts, owned by the asset registry.
    Identity = 0,
    /// Lifecycle state facts, owned by the hardware lifecycle authority.
    Lifecycle = 1,
    /// Maintenance state facts, owned by the maintenance coordinator.
    Maintenance = 2,
    /// Firmware version and baseline facts, owned by the firmware baseline authority.
    Firmware = 3,
    /// Telemetry and fault observations. This observatory owns their interpretation,
    /// not the physical truth they approximate.
    Observation = 4,
};

/// Number of AuthorityDomain enumerators.
inline constexpr std::size_t kAuthorityDomainCount = 5;

/// How much weight an observation's producer claims for it. Precedence between
/// two sources that speak for the same domain is decided by this class first,
/// then by observation time. A synthetic source is never allowed to outrank a
/// measured one, and every assessment that rests on synthetic evidence says so.
enum class SourceClass : std::uint8_t {
    /// Speaks for the authority that owns the domain.
    Authoritative = 0,
    /// A peer producer inside the domain: a second sensor, a second feed.
    Peer = 1,
    /// Modelled or generated evidence. Honest labelling is mandatory.
    Synthetic = 2,
};

/// Number of SourceClass enumerators.
inline constexpr std::size_t kSourceClassCount = 3;

/// Identity of one evidence producer. Stable across restarts of that producer;
/// the stream epoch, not this identifier, changes when the producer restarts.
struct SourceIdTag;
using SourceId = TaggedToken<SourceIdTag>;

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(SourceKind kind) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(AuthorityDomain domain) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(SourceClass klass) noexcept;

/// Canonical, case sensitive parse. Every decoder uses this entry point.
[[nodiscard]] ASSET_HEALTH_API std::optional<SourceKind> parse_source_kind(std::string_view text) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<AuthorityDomain> parse_authority_domain(std::string_view text) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<SourceClass> parse_source_class(std::string_view text) noexcept;

/// The domain a source kind speaks for.
[[nodiscard]] ASSET_HEALTH_API AuthorityDomain authority_domain_of(SourceKind kind) noexcept;

/// True when a source of this kind is allowed to produce evidence whose owning
/// domain is \p domain. The comparison is exact: "close enough" authority does
/// not exist.
[[nodiscard]] ASSET_HEALTH_API bool may_speak_for(SourceKind kind, AuthorityDomain domain) noexcept;

/// True when this source kind is accepted at ingest at all.
[[nodiscard]] ASSET_HEALTH_API bool is_ingestible(SourceKind kind) noexcept;

/// Everything needed to judge where an observation came from and whether it is
/// still allowed to count.
///
/// The two timestamps are distinct on purpose. observed_at is when the producer
/// says the physical fact was true; received_at is when this observatory admitted
/// it. Freshness is measured from observed_at, and a producer whose observed_at
/// runs ahead of its received_at by more than the policy's skew tolerance is
/// reported as future-dated rather than trusted.
class ASSET_HEALTH_API Provenance {
public:
    Provenance() = default;

    struct Parts {
        SourceId source;
        SourceKind kind = SourceKind::TelemetryFeed;
        SourceClass klass = SourceClass::Peer;
        StreamEpoch epoch;
        StreamSequence sequence;
        Instant observed_at;
        Instant received_at;
        std::optional<VersionText> producer_version;
    };

    /// Validates and builds a provenance value. Rejects an empty source, a
    /// non-ingestible source kind, and a zero sequence number, because each of
    /// those leaves the statement outside any stream that can be replayed and
    /// audited.
    ///
    /// The relationship between observed_at and received_at is deliberately not
    /// judged here. A producer whose clock runs ahead of this observatory's is a
    /// real condition with a policy answer: the observatory refuses a statement
    /// dated beyond its skew tolerance, and the evaluator will not use a
    /// statement dated ahead of the instant under assessment. Both rules are
    /// stated where they are applied; neither is hidden in this constructor.
    [[nodiscard]] static std::optional<Provenance> make(Parts parts) noexcept;

    [[nodiscard]] const SourceId& source() const noexcept { return source_; }
    [[nodiscard]] SourceKind kind() const noexcept { return kind_; }
    [[nodiscard]] SourceClass klass() const noexcept { return klass_; }
    [[nodiscard]] AuthorityDomain domain() const noexcept { return authority_domain_of(kind_); }
    [[nodiscard]] StreamEpoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] StreamSequence sequence() const noexcept { return sequence_; }
    [[nodiscard]] Instant observed_at() const noexcept { return observed_at_; }
    [[nodiscard]] Instant received_at() const noexcept { return received_at_; }
    [[nodiscard]] const std::optional<VersionText>& producer_version() const noexcept { return producer_version_; }

    /// True when this observation claims to be modelled rather than measured.
    [[nodiscard]] bool is_synthetic() const noexcept { return klass_ == SourceClass::Synthetic; }

    /// True when the producer claims to speak for the authority that owns the
    /// domain. It is a claim of provenance, not a grant of authority here.
    [[nodiscard]] bool is_authoritative() const noexcept { return klass_ == SourceClass::Authoritative; }

    /// Canonical single-line spelling used by exports and by the CLI.
    [[nodiscard]] std::string to_string() const;

    /// Total order used to break ties between two observations of the same
    /// subject: newer observation first, then class, then epoch, then sequence,
    /// then source identifier. It is total so that an enumeration over equal
    /// timestamps is still reproducible.
    [[nodiscard]] static bool precedes(const Provenance& lhs, const Provenance& rhs) noexcept;

    [[nodiscard]] bool operator==(const Provenance& other) const noexcept {
        return source_ == other.source_ && kind_ == other.kind_ && klass_ == other.klass_ &&
               epoch_ == other.epoch_ && sequence_ == other.sequence_ && observed_at_ == other.observed_at_ &&
               received_at_ == other.received_at_ && producer_version_ == other.producer_version_;
    }

private:
    SourceId source_{};
    SourceKind kind_ = SourceKind::TelemetryFeed;
    SourceClass klass_ = SourceClass::Peer;
    StreamEpoch epoch_{};
    StreamSequence sequence_{};
    Instant observed_at_{};
    Instant received_at_{};
    std::optional<VersionText> producer_version_{};
};

}  // namespace asset_health

#endif  // ASSET_HEALTH_PROVENANCE_HPP
