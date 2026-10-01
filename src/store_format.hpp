// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The durable format, version 1.
//
// A generation file is a fixed 128-byte header followed by a payload. The header
// names the format version, the generation, the payload length and the payload
// checksum, and its own checksum covers those fields, so a torn write is
// detectable before the payload is touched.
//
// Every enumeration inside the payload is stored as its canonical name rather
// than as its ordinal. That costs bytes and buys the property that renumbering an
// enumeration in a later release cannot silently reinterpret a store written by
// an earlier one, and that a hand-edited file with a name that is not in the
// vocabulary is rejected instead of being mapped onto a neighbouring value.

#ifndef ASSET_HEALTH_SRC_STORE_FORMAT_HPP
#define ASSET_HEALTH_SRC_STORE_FORMAT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "asset_health/error.hpp"
#include "asset_health/persistence.hpp"
#include "asset_health/strong_types.hpp"

namespace asset_health::detail {

/// Bytes of the fixed generation header.
inline constexpr std::size_t kGenerationHeaderBytes = 128;

/// Magic of a generation file.
inline constexpr char kGenerationMagic[8] = {'A', 'H', 'O', 'G', 'E', 'N', '0', '1'};

/// The header of a generation file.
struct GenerationHeader {
    std::uint32_t format_version = limits::kStoreFormatVersion;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::uint64_t payload_length = 0;
    std::uint32_t payload_crc = 0;
    detail::UuidBytes store_id{};
    std::uint64_t previous_generation = 0;
    std::uint64_t previous_payload_digest = 0;
    std::uint64_t pruned_below = 0;
    std::int64_t recorded_at_ns = 0;
};

/// The commit point: the record that names the generation a reader must load.
struct CurrentRecord {
    std::uint32_t format_version = limits::kStoreFormatVersion;
    detail::UuidBytes store_id{};
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    std::uint64_t payload_digest = 0;
};

/// Identity of the store directory, written once when the store is created.
struct MetaRecord {
    std::uint32_t format_version = limits::kStoreFormatVersion;
    detail::UuidBytes store_id{};
    std::int64_t created_at_ns = 0;
};

/// FNV-1a over a payload, used to chain generations and to cross-check CURRENT
/// against the generation it names.
[[nodiscard]] std::uint64_t payload_digest(const std::vector<std::uint8_t>& payload) noexcept;

/// FNV-1a over everything a stream position identifies: the subject, the source
/// and its class, the epoch and sequence, the observation instant and the
/// payload. Two deliveries that claim one stream position agree when their
/// digests agree.
[[nodiscard]] std::uint64_t evidence_digest(const EvidenceRecord& record) noexcept;

/// Encodes a state into a payload.
[[nodiscard]] std::vector<std::uint8_t> encode_payload(const StoreState& state, const GenerationHeader& header);

/// Decodes a payload, validating every record with the same predicate the store
/// applies when it admits one. A payload that decodes is a state the store would
/// have been willing to publish.
[[nodiscard]] Status decode_payload(const std::uint8_t* data, std::size_t size, StoreState& out);

/// Encodes a whole generation file: header then payload.
[[nodiscard]] std::vector<std::uint8_t> encode_generation(const StoreState& state, GenerationHeader header);

/// Decodes a whole file.
///
/// Reports TornTailDiscarded when the file is shorter than its header declares,
/// which is what an interrupted write leaves behind, and PayloadChecksumMismatch
/// or RecordDecodeFailed when a complete file does not agree with itself, which
/// is interior corruption.
[[nodiscard]] Status decode_generation(const std::vector<std::uint8_t>& bytes, StoreState& out,
                                       GenerationHeader& header);

/// Decodes just the header, so an audit can classify a file without decoding its
/// payload.
[[nodiscard]] Status decode_header(const std::vector<std::uint8_t>& bytes, GenerationHeader& header);

[[nodiscard]] std::vector<std::uint8_t> encode_current(const CurrentRecord& record);
[[nodiscard]] Status decode_current(const std::vector<std::uint8_t>& bytes, CurrentRecord& out);
[[nodiscard]] std::vector<std::uint8_t> encode_meta(const MetaRecord& record);
[[nodiscard]] Status decode_meta(const std::vector<std::uint8_t>& bytes, MetaRecord& out);

/// The canonical file name of a generation.
[[nodiscard]] std::string generation_file_name(GenerationSequence generation);
/// Parses a generation file name, returning std::nullopt for anything else.
[[nodiscard]] std::optional<GenerationSequence> parse_generation_file_name(std::string_view name);

}  // namespace asset_health::detail

#endif  // ASSET_HEALTH_SRC_STORE_FORMAT_HPP
