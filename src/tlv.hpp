// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A bounded little-endian encoder and decoder.
//
// The decoder is the trust boundary of this repository: durable files are parsed
// by code that never reads past its buffer. Every read checks the remaining
// length first and records a sticky failure instead of throwing, so a truncated
// or hostile document produces a status, never undefined behaviour.

#ifndef ASSET_HEALTH_SRC_TLV_HPP
#define ASSET_HEALTH_SRC_TLV_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace asset_health::detail {

/// Appends little-endian scalars and length-prefixed blobs to a byte buffer.
class Writer {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void boolean(bool value);

    /// Length-prefixed bytes with a 32-bit length.
    void blob(const void* data, std::size_t length);
    void text(std::string_view value);

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }

private:
    std::vector<std::uint8_t> buffer_;
};

/// Reads the encodings above, bounded by the buffer it was given.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == size_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }

    /// Marks the reader failed without consuming anything.
    void fail() noexcept { ok_ = false; }

    [[nodiscard]] std::uint8_t u8() noexcept;
    [[nodiscard]] std::uint16_t u16() noexcept;
    [[nodiscard]] std::uint32_t u32() noexcept;
    [[nodiscard]] std::uint64_t u64() noexcept;
    [[nodiscard]] std::int64_t i64() noexcept;
    [[nodiscard]] bool boolean() noexcept;

    /// Reads a length-prefixed blob. The declared length is checked against the
    /// remaining input before anything is read or allocated.
    [[nodiscard]] bool blob(std::string& out, std::size_t max_length) noexcept;
    [[nodiscard]] bool blob(std::vector<std::uint8_t>& out, std::size_t max_length) noexcept;
    [[nodiscard]] bool text(std::string& out, std::size_t max_length) noexcept;
    [[nodiscard]] bool skip(std::size_t length) noexcept;
    /// True when the next bytes equal \p expected; consumes them when they do.
    [[nodiscard]] bool expect(const void* expected, std::size_t length) noexcept;

private:
    [[nodiscard]] bool require(std::size_t length) noexcept;

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

}  // namespace asset_health::detail

#endif  // ASSET_HEALTH_SRC_TLV_HPP
