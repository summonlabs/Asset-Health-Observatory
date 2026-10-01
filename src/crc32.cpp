// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "crc32.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace asset_health::detail {
namespace {

/// The table is built once, at first use, from the reflected polynomial. Building
/// it rather than embedding 256 constants keeps the polynomial visible in the
/// source and removes any chance of a transcription error in the table.
struct Table {
    std::array<std::uint32_t, 256> values{};

    constexpr Table() noexcept {
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            values[index] = value;
        }
    }
};

constexpr Table kTable{};

}  // namespace

std::uint32_t crc32_update(std::uint32_t seed, const void* data, std::size_t length) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t value = seed;
    for (std::size_t index = 0; index < length; ++index) {
        value = kTable.values[(value ^ bytes[index]) & 0xFFu] ^ (value >> 8);
    }
    return value;
}

std::uint32_t crc32_bytes(const void* data, std::size_t length) noexcept {
    return crc32_update(0xFFFFFFFFu, data, length) ^ 0xFFFFFFFFu;
}

}  // namespace asset_health::detail
