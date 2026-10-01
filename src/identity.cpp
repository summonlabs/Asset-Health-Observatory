// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/identity.hpp"

#include <cstddef>

namespace asset_health {

const AssetRefId& AssetRefId::nil() noexcept {
    static const AssetRefId value;
    return value;
}

AssetRefId AssetRefId::from_bytes(const bytes_type& bytes) noexcept {
    AssetRefId value;
    value.bytes_ = bytes;
    return value;
}

std::optional<AssetRefId> AssetRefId::parse(std::string_view text) noexcept {
    const auto bytes = parse_uuid_text(text);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return AssetRefId::from_bytes(*bytes);
}

bool AssetRefId::is_nil() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string AssetRefId::to_string() const { return format_uuid_text(bytes_); }

std::string AssetRefId::to_compact_string() const { return format_uuid_compact(bytes_); }

std::optional<AssetReference> AssetReference::make(AssetRefId id, AssetGeneration generation,
                                                   AssetRevision revision) noexcept {
    if (id.is_nil() || generation.is_zero() || revision.is_zero()) {
        return std::nullopt;
    }
    return AssetReference(id, generation, revision);
}

std::string AssetReference::to_string() const {
    return id_.to_string() + "@g" + generation_.to_string() + "r" + revision_.to_string();
}

std::optional<EvidenceSubject> EvidenceSubject::make(AssetRefId asset, AssetGeneration generation) noexcept {
    if (asset.is_nil() || generation.is_zero()) {
        return std::nullopt;
    }
    EvidenceSubject subject(asset);
    subject.generation_ = generation;
    return subject;
}

}  // namespace asset_health
