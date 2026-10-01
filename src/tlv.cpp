// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "tlv.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace asset_health::detail {

void Writer::u8(std::uint8_t value) { buffer_.push_back(value); }

void Writer::u16(std::uint16_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void Writer::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void Writer::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void Writer::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void Writer::boolean(bool value) { u8(value ? 1u : 0u); }

void Writer::blob(const void* data, std::size_t length) {
    u32(static_cast<std::uint32_t>(length));
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + length);
}

void Writer::text(std::string_view value) { blob(value.data(), value.size()); }

bool Reader::require(std::size_t length) noexcept {
    if (!ok_) {
        return false;
    }
    if (length > size_ - offset_) {
        ok_ = false;
        return false;
    }
    return true;
}

std::uint8_t Reader::u8() noexcept {
    if (!require(1)) {
        return 0;
    }
    return data_[offset_++];
}

std::uint16_t Reader::u16() noexcept {
    if (!require(2)) {
        return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(data_[offset_]) |
                                static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
    offset_ += 2;
    return value;
}

std::uint32_t Reader::u32() noexcept {
    if (!require(4)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(index)]) << (8 * index);
    }
    offset_ += 4;
    return value;
}

std::uint64_t Reader::u64() noexcept {
    if (!require(8)) {
        return 0;
    }
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(index)]) << (8 * index);
    }
    offset_ += 8;
    return value;
}

std::int64_t Reader::i64() noexcept { return static_cast<std::int64_t>(u64()); }

bool Reader::boolean() noexcept { return u8() != 0; }

bool Reader::blob(std::vector<std::uint8_t>& out, std::size_t max_length) noexcept {
    const std::uint32_t declared = u32();
    if (!ok_) {
        return false;
    }
    if (declared > max_length) {
        ok_ = false;
        return false;
    }
    if (!require(declared)) {
        return false;
    }
    out.assign(data_ + offset_, data_ + offset_ + declared);
    offset_ += declared;
    return true;
}

bool Reader::blob(std::string& out, std::size_t max_length) noexcept {
    const std::uint32_t declared = u32();
    if (!ok_) {
        return false;
    }
    if (declared > max_length) {
        ok_ = false;
        return false;
    }
    if (!require(declared)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + offset_), declared);
    offset_ += declared;
    return true;
}

bool Reader::text(std::string& out, std::size_t max_length) noexcept { return blob(out, max_length); }

bool Reader::skip(std::size_t length) noexcept {
    if (!require(length)) {
        return false;
    }
    offset_ += length;
    return true;
}

bool Reader::expect(const void* expected, std::size_t length) noexcept {
    if (!require(length)) {
        return false;
    }
    if (std::memcmp(data_ + offset_, expected, length) != 0) {
        ok_ = false;
        return false;
    }
    offset_ += length;
    return true;
}

}  // namespace asset_health::detail
