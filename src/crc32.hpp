// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// CRC-32 (the reflected IEEE polynomial used by every standard tool). It is an
// integrity check, not a security control: it detects a torn write, a truncated
// file and the bit rot that follows a failing disk. Authenticity is not claimed
// and is not needed, because the store directory is the operator's.

#ifndef ASSET_HEALTH_SRC_CRC32_HPP
#define ASSET_HEALTH_SRC_CRC32_HPP

#include <cstddef>
#include <cstdint>

namespace asset_health::detail {

/// Continues a CRC-32 over \p length bytes.
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t seed, const void* data, std::size_t length) noexcept;

/// CRC-32 of a byte range, starting from the standard initial value.
[[nodiscard]] std::uint32_t crc32_bytes(const void* data, std::size_t length) noexcept;

}  // namespace asset_health::detail

#endif  // ASSET_HEALTH_SRC_CRC32_HPP
