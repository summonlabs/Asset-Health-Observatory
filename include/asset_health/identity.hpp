// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Asset identity as this observatory sees it: a reference to an asset that some
// other authority owns, plus the generation and revision that authority had
// published when the reference was observed.
//
// Non-ownership is explicit in the types. AssetRefId carries no way to mint an
// asset, bump a generation, or move a lifecycle state; it is a value this
// observatory read from evidence and can quote back. The canonical spelling is
// the lowercase hyphenated 128-bit form already published by the DCCP identity
// authority, so the text crosses the boundary unchanged; the parsing, validation
// and any decision made from it here are this repository's own.

#ifndef ASSET_HEALTH_IDENTITY_HPP
#define ASSET_HEALTH_IDENTITY_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_health/error.hpp"
#include "asset_health/strong_types.hpp"

namespace asset_health {

/// The identity of the hardware object this observatory observes.
class ASSET_HEALTH_API AssetRefId {
public:
    using bytes_type = detail::UuidBytes;

    /// The all-zero identifier. A real value meaning "the nil identifier", never
    /// a stand-in for "no identifier"; absence is std::optional<AssetRefId>.
    [[nodiscard]] static const AssetRefId& nil() noexcept;

    /// Builds an identifier from 16 bytes. Used by decoders and by tests; the
    /// product path reads identities from evidence.
    [[nodiscard]] static AssetRefId from_bytes(const bytes_type& bytes) noexcept;

    /// Parses the canonical lowercase hyphenated form. Every other spelling is
    /// rejected.
    [[nodiscard]] static std::optional<AssetRefId> parse(std::string_view text) noexcept;

    [[nodiscard]] const bytes_type& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_nil() const noexcept;
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string to_compact_string() const;

    [[nodiscard]] friend bool operator==(const AssetRefId& lhs, const AssetRefId& rhs) noexcept {
        return lhs.bytes_ == rhs.bytes_;
    }
    [[nodiscard]] friend bool operator!=(const AssetRefId& lhs, const AssetRefId& rhs) noexcept {
        return !(lhs == rhs);
    }
    /// Total order by raw bytes, most significant first. This is the canonical
    /// enumeration order of every public listing.
    [[nodiscard]] friend bool operator<(const AssetRefId& lhs, const AssetRefId& rhs) noexcept {
        return lhs.bytes_ < rhs.bytes_;
    }

private:
    bytes_type bytes_{};
};

/// Incarnation number of the identity, as published by the identity authority.
/// Generation 1 is the first incarnation. A later generation means the identity
/// was reissued to different physical hardware, which makes every observation
/// bound to an earlier generation a statement about a different object.
struct AssetGenerationTag;
using AssetGeneration = detail::Counter<AssetGenerationTag, std::uint32_t>;

/// Revision of the identity record observed at the same time as the generation.
/// Revision 0 is "no revision observed" and is never produced by a successful
/// identity observation.
struct AssetRevisionTag;
using AssetRevision = detail::Counter<AssetRevisionTag, std::uint64_t>;

/// One observation of "which object, which incarnation, which revision".
class ASSET_HEALTH_API AssetReference {
public:
    AssetReference() = default;
    AssetReference(AssetRefId id, AssetGeneration generation, AssetRevision revision) noexcept
        : id_(id), generation_(generation), revision_(revision) {}

    [[nodiscard]] static std::optional<AssetReference> make(AssetRefId id, AssetGeneration generation,
                                                            AssetRevision revision) noexcept;

    [[nodiscard]] const AssetRefId& id() const noexcept { return id_; }
    [[nodiscard]] AssetGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] AssetRevision revision() const noexcept { return revision_; }

    /// True when the reference names a real object: a non-nil identity in a real
    /// generation at a real revision.
    [[nodiscard]] bool is_valid() const noexcept {
        return !id_.is_nil() && !generation_.is_zero() && !revision_.is_zero();
    }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend bool operator==(const AssetReference& lhs, const AssetReference& rhs) noexcept {
        return lhs.id_ == rhs.id_ && lhs.generation_ == rhs.generation_ && lhs.revision_ == rhs.revision_;
    }
    [[nodiscard]] friend bool operator!=(const AssetReference& lhs, const AssetReference& rhs) noexcept {
        return !(lhs == rhs);
    }
    /// Order by identity, then generation, then revision.
    [[nodiscard]] friend bool operator<(const AssetReference& lhs, const AssetReference& rhs) noexcept {
        if (lhs.id_ != rhs.id_) {
            return lhs.id_ < rhs.id_;
        }
        if (lhs.generation_ != rhs.generation_) {
            return lhs.generation_ < rhs.generation_;
        }
        return lhs.revision_ < rhs.revision_;
    }

private:
    AssetRefId id_{};
    AssetGeneration generation_{};
    AssetRevision revision_{};
};

/// Identity of a part inside an asset, as named by the producer of the evidence
/// (for example "psu-1", "gpu-0", "dimm-a2"). The observatory does not own this
/// namespace and does not resolve it against a bill of materials; it groups
/// observations by the component name the producer used and reports that name.
struct ComponentIdTag;
using ComponentId = TaggedToken<ComponentIdTag>;

/// The identity half of an evidence record: the object and incarnation an
/// observation is about.
class ASSET_HEALTH_API EvidenceSubject {
public:
    EvidenceSubject() = default;
    explicit EvidenceSubject(AssetRefId asset) noexcept : asset_(asset) {}

    [[nodiscard]] const AssetRefId& asset() const noexcept { return asset_; }
    [[nodiscard]] AssetGeneration generation() const noexcept { return generation_; }

    /// Binds the observation to an incarnation. Generation 0 is rejected: an
    /// observation that does not name the incarnation it observed cannot be
    /// placed in the object's history, and guessing one would silently mix two
    /// physical objects.
    [[nodiscard]] static std::optional<EvidenceSubject> make(AssetRefId asset, AssetGeneration generation) noexcept;

    [[nodiscard]] friend bool operator==(const EvidenceSubject& lhs, const EvidenceSubject& rhs) noexcept {
        return lhs.asset_ == rhs.asset_ && lhs.generation_ == rhs.generation_;
    }
    [[nodiscard]] friend bool operator!=(const EvidenceSubject& lhs, const EvidenceSubject& rhs) noexcept {
        return !(lhs == rhs);
    }
    [[nodiscard]] friend bool operator<(const EvidenceSubject& lhs, const EvidenceSubject& rhs) noexcept {
        if (lhs.asset_ != rhs.asset_) {
            return lhs.asset_ < rhs.asset_;
        }
        return lhs.generation_ < rhs.generation_;
    }

private:
    AssetRefId asset_{};
    AssetGeneration generation_{};
};

}  // namespace asset_health

#endif  // ASSET_HEALTH_IDENTITY_HPP
