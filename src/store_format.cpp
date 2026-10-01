// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "store_format.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_health/limits.hpp"
#include "crc32.hpp"
#include "tlv.hpp"

namespace asset_health::detail {
namespace {

constexpr std::uint32_t kPayloadMagic = 0x504F4841u;  // "AHOP" little endian
constexpr std::uint32_t kPayloadVersion = 1;
constexpr std::uint32_t kCurrentMagic = 0x52554341u;  // "ACUR"
constexpr std::uint32_t kMetaMagic = 0x4154454Du;     // "META"

constexpr std::size_t kMaxEncodedText = limits::kMaxDetailTextLength;

void write_uuid(Writer& writer, const UuidBytes& bytes) { writer.blob(bytes.data(), bytes.size()); }

[[nodiscard]] bool read_uuid(Reader& reader, UuidBytes& out) {
    std::vector<std::uint8_t> bytes;
    if (!reader.blob(bytes, 16)) {
        return false;
    }
    if (bytes.size() != 16) {
        reader.fail();
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return true;
}

void write_text(Writer& writer, std::string_view value) { writer.text(value); }

void write_optional_text(Writer& writer, const std::optional<DetailText>& value) {
    writer.boolean(value.has_value());
    if (value.has_value()) {
        write_text(writer, value->view());
    }
}

[[nodiscard]] bool read_optional_text(Reader& reader, std::optional<DetailText>& out, std::size_t max_length) {
    const bool present = reader.boolean();
    if (!reader.ok()) {
        return false;
    }
    if (!present) {
        out.reset();
        return true;
    }
    std::string text;
    if (!reader.text(text, max_length)) {
        return false;
    }
    const auto parsed = DetailText::parse(text, max_length);
    if (!parsed.has_value()) {
        reader.fail();
        return false;
    }
    out = *parsed;
    return true;
}

void write_optional_instant(Writer& writer, const std::optional<Instant>& value) {
    writer.boolean(value.has_value());
    writer.i64(value.has_value() ? value->unix_nanoseconds() : 0);
}

[[nodiscard]] bool read_optional_instant(Reader& reader, std::optional<Instant>& out) {
    const bool present = reader.boolean();
    const std::int64_t value = reader.i64();
    if (!reader.ok()) {
        return false;
    }
    if (present) {
        out = Instant(value);
    } else {
        out.reset();
    }
    return true;
}

void write_optional_version(Writer& writer, const std::optional<VersionText>& value) {
    writer.boolean(value.has_value());
    if (value.has_value()) {
        write_text(writer, value->view());
    }
}

[[nodiscard]] bool read_optional_version(Reader& reader, std::optional<VersionText>& out) {
    const bool present = reader.boolean();
    if (!reader.ok()) {
        return false;
    }
    if (!present) {
        out.reset();
        return true;
    }
    std::string text;
    if (!reader.text(text, limits::kMaxVersionTextLength)) {
        return false;
    }
    const auto parsed = VersionText::parse(text);
    if (!parsed.has_value()) {
        reader.fail();
        return false;
    }
    out = *parsed;
    return true;
}

void write_payload(Writer& writer, const EvidencePayload& payload) {
    writer.u8(static_cast<std::uint8_t>(payload.kind));
    switch (payload.kind) {
        case EvidenceKind::IdentityStatement:
            writer.u64(payload.identity.revision.value());
            write_optional_text(writer, payload.identity.serial);
            writer.boolean(payload.identity.current);
            break;
        case EvidenceKind::LifecycleStatement:
            write_text(writer, to_string(payload.lifecycle.state));
            write_optional_instant(writer, payload.lifecycle.effective_at);
            break;
        case EvidenceKind::MaintenanceStatement:
            write_text(writer, to_string(payload.maintenance.kind));
            write_text(writer, to_string(payload.maintenance.state));
            writer.i64(payload.maintenance.window_start.unix_nanoseconds());
            writer.i64(payload.maintenance.window_end.unix_nanoseconds());
            write_text(writer, to_string(payload.maintenance.mask));
            write_optional_text(writer, payload.maintenance.reference);
            break;
        case EvidenceKind::FirmwareStatement:
            write_text(writer, payload.firmware.component.str());
            write_text(writer, payload.firmware.version.str());
            write_optional_version(writer, payload.firmware.baseline);
            write_text(writer, to_string(payload.firmware.compliance));
            break;
        case EvidenceKind::TelemetryReading:
            write_text(writer, to_string(payload.telemetry.metric));
            writer.i64(payload.telemetry.value.milli());
            write_text(writer, to_string(payload.telemetry.value.unit()));
            write_text(writer, to_string(payload.telemetry.quality));
            writer.boolean(payload.telemetry.cached);
            break;
        case EvidenceKind::FaultStatement:
            write_text(writer, payload.fault.component.str());
            write_text(writer, payload.fault.code.str());
            write_text(writer, to_string(payload.fault.severity));
            write_text(writer, to_string(payload.fault.status));
            write_optional_instant(writer, payload.fault.first_seen);
            write_optional_text(writer, payload.fault.description);
            break;
    }
}

[[nodiscard]] bool read_token_field(Reader& reader, Token& out) {
    std::string text;
    if (!reader.text(text, limits::kMaxTokenLength)) {
        return false;
    }
    const auto parsed = Token::parse(text);
    if (!parsed.has_value()) {
        reader.fail();
        return false;
    }
    out = *parsed;
    return true;
}

template <typename Tag>
[[nodiscard]] bool read_tagged_token(Reader& reader, TaggedToken<Tag>& out) {
    std::string text;
    if (!reader.text(text, limits::kMaxTokenLength)) {
        return false;
    }
    const auto parsed = TaggedToken<Tag>::parse(text);
    if (!parsed.has_value()) {
        reader.fail();
        return false;
    }
    out = *parsed;
    return true;
}

template <typename Enum, typename ParseFunction>
[[nodiscard]] bool read_enum(Reader& reader, Enum& out, ParseFunction parse) {
    std::string text;
    if (!reader.text(text, limits::kMaxTokenLength)) {
        return false;
    }
    const auto parsed = parse(text);
    if (!parsed.has_value()) {
        // A name outside the vocabulary is corruption, not a value to guess at.
        reader.fail();
        return false;
    }
    out = *parsed;
    return true;
}

[[nodiscard]] bool read_payload(Reader& reader, EvidencePayload& out) {
    const auto kind = static_cast<EvidenceKind>(reader.u8());
    if (!reader.ok() || static_cast<std::size_t>(kind) >= kEvidenceKindCount) {
        reader.fail();
        return false;
    }
    out = EvidencePayload{};
    out.kind = kind;
    switch (kind) {
        case EvidenceKind::IdentityStatement: {
            const std::uint64_t revision = reader.u64();
            out.identity.revision = AssetRevision(revision);
            if (!read_optional_text(reader, out.identity.serial, limits::kMaxLabelTextLength)) {
                return false;
            }
            out.identity.current = reader.boolean();
            break;
        }
        case EvidenceKind::LifecycleStatement:
            if (!read_enum(reader, out.lifecycle.state, &parse_lifecycle_state)) {
                return false;
            }
            if (!read_optional_instant(reader, out.lifecycle.effective_at)) {
                return false;
            }
            break;
        case EvidenceKind::MaintenanceStatement:
            if (!read_enum(reader, out.maintenance.kind, &parse_maintenance_kind) ||
                !read_enum(reader, out.maintenance.state, &parse_maintenance_state)) {
                return false;
            }
            out.maintenance.window_start = Instant(reader.i64());
            out.maintenance.window_end = Instant(reader.i64());
            if (!read_enum(reader, out.maintenance.mask, &parse_mask_scope)) {
                return false;
            }
            if (!read_optional_text(reader, out.maintenance.reference, limits::kMaxLabelTextLength)) {
                return false;
            }
            break;
        case EvidenceKind::FirmwareStatement:
            if (!read_tagged_token(reader, out.firmware.component)) {
                return false;
            }
            {
                std::string version;
                if (!reader.text(version, limits::kMaxVersionTextLength)) {
                    return false;
                }
                const auto parsed = VersionText::parse(version);
                if (!parsed.has_value()) {
                    reader.fail();
                    return false;
                }
                out.firmware.version = *parsed;
            }
            if (!read_optional_version(reader, out.firmware.baseline)) {
                return false;
            }
            if (!read_enum(reader, out.firmware.compliance, &parse_firmware_compliance)) {
                return false;
            }
            break;
        case EvidenceKind::TelemetryReading:
            if (!read_enum(reader, out.telemetry.metric, &parse_metric)) {
                return false;
            }
            {
                const std::int64_t milli = reader.i64();
                Unit unit = Unit::None;
                if (!read_enum(reader, unit, &parse_unit)) {
                    return false;
                }
                const auto quantity = Quantity::make(unit, milli);
                if (!quantity.has_value()) {
                    reader.fail();
                    return false;
                }
                out.telemetry.value = *quantity;
            }
            if (!read_enum(reader, out.telemetry.quality, &parse_sample_quality)) {
                return false;
            }
            out.telemetry.cached = reader.boolean();
            break;
        case EvidenceKind::FaultStatement:
            if (!read_tagged_token(reader, out.fault.component) || !read_tagged_token(reader, out.fault.code)) {
                return false;
            }
            if (!read_enum(reader, out.fault.severity, &parse_fault_severity) ||
                !read_enum(reader, out.fault.status, &parse_fault_status)) {
                return false;
            }
            if (!read_optional_instant(reader, out.fault.first_seen)) {
                return false;
            }
            if (!read_optional_text(reader, out.fault.description, kMaxEncodedText)) {
                return false;
            }
            break;
    }
    return reader.ok();
}

void write_evidence(Writer& writer, const EvidenceRecord& record) {
    write_uuid(writer, record.id().bytes());
    write_uuid(writer, record.subject().asset().bytes());
    writer.u32(record.subject().generation().value());
    writer.u64(record.commit_epoch().value());
    write_text(writer, record.source().str());
    write_text(writer, to_string(record.source_kind()));
    write_text(writer, to_string(record.source_class()));
    writer.u64(record.provenance().epoch().value());
    writer.u64(record.provenance().sequence().value());
    writer.i64(record.observed_at().unix_nanoseconds());
    writer.i64(record.received_at().unix_nanoseconds());
    write_optional_version(writer, record.provenance().producer_version());
    write_payload(writer, record.payload());
}

[[nodiscard]] bool read_evidence(Reader& reader, EvidenceRecord& out) {
    UuidBytes id_bytes{};
    UuidBytes asset_bytes{};
    if (!read_uuid(reader, id_bytes) || !read_uuid(reader, asset_bytes)) {
        return false;
    }
    const std::uint32_t generation = reader.u32();
    const std::uint64_t commit_epoch = reader.u64();
    Provenance::Parts provenance;
    if (!read_tagged_token(reader, provenance.source)) {
        return false;
    }
    if (!read_enum(reader, provenance.kind, &parse_source_kind) ||
        !read_enum(reader, provenance.klass, &parse_source_class)) {
        return false;
    }
    provenance.epoch = StreamEpoch(reader.u64());
    provenance.sequence = StreamSequence(reader.u64());
    provenance.observed_at = Instant(reader.i64());
    provenance.received_at = Instant(reader.i64());
    if (!read_optional_version(reader, provenance.producer_version)) {
        return false;
    }
    if (!reader.ok()) {
        return false;
    }
    const auto provenance_value = Provenance::make(std::move(provenance));
    if (!provenance_value.has_value()) {
        reader.fail();
        return false;
    }
    const auto subject = EvidenceSubject::make(AssetRefId::from_bytes(asset_bytes), AssetGeneration(generation));
    if (!subject.has_value()) {
        reader.fail();
        return false;
    }
    EvidencePayload payload;
    if (!read_payload(reader, payload)) {
        return false;
    }
    EvidenceRecord::Parts parts;
    parts.id = EvidenceId::from_bytes(id_bytes);
    parts.subject = *subject;
    parts.provenance = *provenance_value;
    parts.payload = payload;
    const auto record = EvidenceRecord::make(std::move(parts));
    if (!record.has_value()) {
        reader.fail();
        return false;
    }
    out = record.value().with_commit_epoch(StoreEpoch(commit_epoch));
    return true;
}

void write_history(Writer& writer, const HistoryEntry& entry) {
    write_uuid(writer, entry.asset.bytes());
    writer.u32(entry.generation.value());
    writer.u64(entry.revision.value());
    write_text(writer, to_string(entry.state));
    write_text(writer, to_string(entry.risk_band));
    writer.u32(entry.flags.bits());
    writer.i64(entry.evaluated_at.unix_nanoseconds());
    writer.i64(entry.recorded_at.unix_nanoseconds());
    write_text(writer, entry.policy_fingerprint);
    writer.u16(entry.finding_count);
    writer.u16(entry.attention_findings);
    writer.u32(entry.evidence_depended_on);
}

[[nodiscard]] bool read_history(Reader& reader, HistoryEntry& out) {
    UuidBytes asset_bytes{};
    if (!read_uuid(reader, asset_bytes)) {
        return false;
    }
    out.asset = AssetRefId::from_bytes(asset_bytes);
    out.generation = AssetGeneration(reader.u32());
    out.revision = AssessmentRevision(reader.u64());
    if (!read_enum(reader, out.state, &parse_health_state) ||
        !read_enum(reader, out.risk_band, &parse_risk_band)) {
        return false;
    }
    out.flags = AssessmentFlags(reader.u32());
    out.evaluated_at = Instant(reader.i64());
    out.recorded_at = Instant(reader.i64());
    if (!reader.text(out.policy_fingerprint, 32)) {
        return false;
    }
    out.finding_count = reader.u16();
    out.attention_findings = reader.u16();
    out.evidence_depended_on = reader.u32();
    return reader.ok();
}

void write_refusal(Writer& writer, const RefusalRecord& refusal) {
    writer.boolean(refusal.asset.has_value());
    write_uuid(writer, refusal.asset.has_value() ? refusal.asset->bytes() : UuidBytes{});
    writer.u32(refusal.generation.value());
    write_text(writer, refusal.source.str());
    write_text(writer, to_string(refusal.source_kind));
    writer.boolean(refusal.claimed_kind.has_value());
    writer.u8(refusal.claimed_kind.has_value() ? static_cast<std::uint8_t>(*refusal.claimed_kind) : 0xFFu);
    writer.u16(static_cast<std::uint16_t>(refusal.code));
    write_text(writer, refusal.detail);
    writer.i64(refusal.received_at.unix_nanoseconds());
}

[[nodiscard]] bool read_refusal(Reader& reader, RefusalRecord& out) {
    const bool has_asset = reader.boolean();
    UuidBytes asset_bytes{};
    if (!read_uuid(reader, asset_bytes)) {
        return false;
    }
    if (has_asset) {
        out.asset = AssetRefId::from_bytes(asset_bytes);
    }
    out.generation = AssetGeneration(reader.u32());
    if (!read_tagged_token(reader, out.source)) {
        return false;
    }
    if (!read_enum(reader, out.source_kind, &parse_source_kind)) {
        return false;
    }
    const bool has_kind = reader.boolean();
    const auto kind = static_cast<EvidenceKind>(reader.u8());
    if (!reader.ok()) {
        return false;
    }
    if (has_kind) {
        if (static_cast<std::size_t>(kind) >= kEvidenceKindCount) {
            reader.fail();
            return false;
        }
        out.claimed_kind = kind;
    }
    const std::uint16_t code = reader.u16();
    if (!reader.ok() || code >= kErrorCodeCount) {
        reader.fail();
        return false;
    }
    out.code = static_cast<ErrorCode>(code);
    if (!reader.text(out.detail, limits::kMaxDetailTextLength)) {
        return false;
    }
    out.received_at = Instant(reader.i64());
    return reader.ok();
}

void write_retry(Writer& writer, const RetryRecord& record) {
    writer.u64(record.sequence.value());
    write_text(writer, record.idempotency_key);
    writer.u64(record.resulting_generation.value());
    writer.i64(record.committed_at.unix_nanoseconds());
}

[[nodiscard]] bool read_retry(Reader& reader, RetryRecord& out) {
    out.sequence = GenerationSequence(reader.u64());
    if (!reader.text(out.idempotency_key, limits::kMaxDetailTextLength)) {
        return false;
    }
    out.resulting_generation = GenerationSequence(reader.u64());
    out.committed_at = Instant(reader.i64());
    return reader.ok();
}

void write_stream(Writer& writer, const SourceStreamState& stream) {
    write_text(writer, stream.source.str());
    write_text(writer, to_string(stream.kind));
    writer.u64(stream.epoch.value());
    writer.u64(stream.last_sequence.value());
    writer.u64(stream.last_digest);
    writer.i64(stream.last_observed_at.unix_nanoseconds());
    writer.i64(stream.last_received_at.unix_nanoseconds());
    write_uuid(writer, stream.last_evidence_id.bytes());
}

[[nodiscard]] bool read_stream(Reader& reader, SourceStreamState& out) {
    if (!read_tagged_token(reader, out.source)) {
        return false;
    }
    if (!read_enum(reader, out.kind, &parse_source_kind)) {
        return false;
    }
    out.epoch = StreamEpoch(reader.u64());
    out.last_sequence = StreamSequence(reader.u64());
    out.last_digest = reader.u64();
    out.last_observed_at = Instant(reader.i64());
    out.last_received_at = Instant(reader.i64());
    UuidBytes evidence_bytes{};
    if (!read_uuid(reader, evidence_bytes)) {
        return false;
    }
    out.last_evidence_id = EvidenceId::from_bytes(evidence_bytes);
    return reader.ok();
}

}  // namespace

std::uint64_t payload_digest(const std::vector<std::uint8_t>& payload) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : payload) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t evidence_digest(const EvidenceRecord& record) noexcept {
    Writer writer;
    write_uuid(writer, record.subject().asset().bytes());
    writer.u32(record.subject().generation().value());
    write_text(writer, record.source().str());
    write_text(writer, to_string(record.source_kind()));
    write_text(writer, to_string(record.source_class()));
    writer.u64(record.provenance().epoch().value());
    writer.u64(record.provenance().sequence().value());
    writer.i64(record.observed_at().unix_nanoseconds());
    write_payload(writer, record.payload());
    const std::vector<std::uint8_t>& bytes = writer.bytes();
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::uint8_t> encode_payload(const StoreState& state, const GenerationHeader& header) {
    Writer writer;
    writer.u32(kPayloadMagic);
    writer.u32(kPayloadVersion);
    write_uuid(writer, header.store_id);
    writer.u64(header.generation);
    writer.u64(header.previous_generation);
    writer.u64(header.previous_payload_digest);
    writer.u64(header.pruned_below);
    writer.u32(static_cast<std::uint32_t>(state.evidence.size()));
    for (const EvidenceRecord& record : state.evidence) {
        write_evidence(writer, record);
    }
    writer.u32(static_cast<std::uint32_t>(state.history.size()));
    for (const HistoryEntry& entry : state.history) {
        write_history(writer, entry);
    }
    writer.u32(static_cast<std::uint32_t>(state.refusals.size()));
    for (const RefusalRecord& refusal : state.refusals) {
        write_refusal(writer, refusal);
    }
    writer.u32(static_cast<std::uint32_t>(state.retries.size()));
    for (const RetryRecord& record : state.retries) {
        write_retry(writer, record);
    }
    writer.u32(static_cast<std::uint32_t>(state.source_streams.size()));
    for (const SourceStreamState& stream : state.source_streams) {
        write_stream(writer, stream);
    }
    return writer.take();
}

Status decode_payload(const std::uint8_t* data, std::size_t size, StoreState& out) {
    Reader reader(data, size);
    if (reader.u32() != kPayloadMagic) {
        return Error(ErrorCode::StoreLayoutInvalid, "the payload magic does not match this format");
    }
    const std::uint32_t version = reader.u32();
    if (!reader.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the payload header is truncated");
    }
    if (version != kPayloadVersion) {
        return Error(ErrorCode::UnsupportedFormatVersion, "the payload version is not supported by this build")
            .with_context("payload_version", std::to_string(version));
    }
    if (!read_uuid(reader, out.store_id)) {
        return Error(ErrorCode::InteriorCorruption, "the store identifier is truncated");
    }
    out.generation = GenerationSequence(reader.u64());
    out.previous_generation = GenerationSequence(reader.u64());
    out.previous_payload_digest = reader.u64();
    out.pruned_below = GenerationSequence(reader.u64());

    const std::uint32_t evidence_count = reader.u32();
    if (!reader.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the evidence count is truncated");
    }
    if (evidence_count > limits::kMaxEvidenceTotal) {
        return Error(ErrorCode::PayloadTooLarge, "the declared evidence count exceeds the configured bound")
            .with_context("declared", std::to_string(evidence_count));
    }
    out.evidence.reserve(std::min<std::size_t>(evidence_count, 1024));
    for (std::uint32_t index = 0; index < evidence_count; ++index) {
        EvidenceRecord record;
        if (!read_evidence(reader, record)) {
            return Error(ErrorCode::RecordDecodeFailed, "an evidence record could not be decoded")
                .with_context("index", std::to_string(index));
        }
        out.evidence.push_back(std::move(record));
    }

    const std::uint32_t history_count = reader.u32();
    if (!reader.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the history count is truncated");
    }
    if (history_count > limits::kMaxHistoryTotal) {
        return Error(ErrorCode::PayloadTooLarge, "the declared history count exceeds the configured bound");
    }
    for (std::uint32_t index = 0; index < history_count; ++index) {
        HistoryEntry entry;
        if (!read_history(reader, entry)) {
            return Error(ErrorCode::RecordDecodeFailed, "a history entry could not be decoded")
                .with_context("index", std::to_string(index));
        }
        out.history.push_back(std::move(entry));
    }

    const std::uint32_t refusal_count = reader.u32();
    if (!reader.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the refusal count is truncated");
    }
    if (refusal_count > limits::kMaxRefusalsTotal) {
        return Error(ErrorCode::PayloadTooLarge, "the declared refusal count exceeds the configured bound");
    }
    for (std::uint32_t index = 0; index < refusal_count; ++index) {
        RefusalRecord refusal;
        if (!read_refusal(reader, refusal)) {
            return Error(ErrorCode::RecordDecodeFailed, "a refusal record could not be decoded");
        }
        out.refusals.push_back(std::move(refusal));
    }

    const std::uint32_t retry_count = reader.u32();
    if (!reader.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the retry count is truncated");
    }
    if (retry_count > limits::kMaxRetryRecords) {
        return Error(ErrorCode::PayloadTooLarge, "the declared retry count exceeds the configured bound");
    }
    for (std::uint32_t index = 0; index < retry_count; ++index) {
        RetryRecord record;
        if (!read_retry(reader, record)) {
            return Error(ErrorCode::RecordDecodeFailed, "a retry record could not be decoded");
        }
        out.retries.push_back(std::move(record));
    }

    const std::uint32_t stream_count = reader.u32();
    if (!reader.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the stream count is truncated");
    }
    if (stream_count > limits::kMaxStreamsTotal) {
        return Error(ErrorCode::PayloadTooLarge, "the declared stream count exceeds the configured bound");
    }
    for (std::uint32_t index = 0; index < stream_count; ++index) {
        SourceStreamState stream;
        if (!read_stream(reader, stream)) {
            return Error(ErrorCode::RecordDecodeFailed, "a source stream record could not be decoded");
        }
        out.source_streams.push_back(std::move(stream));
    }

    if (!reader.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the payload ended before its declared content did");
    }
    if (!reader.at_end()) {
        return Error(ErrorCode::InteriorCorruption, "the payload carries trailing bytes")
            .with_context("remaining", std::to_string(reader.remaining()));
    }
    return ok();
}

std::vector<std::uint8_t> encode_generation(const StoreState& state, GenerationHeader header) {
    std::vector<std::uint8_t> payload = encode_payload(state, header);
    header.payload_length = payload.size();
    header.payload_crc = crc32_bytes(payload.data(), payload.size());

    // The header layout is fixed and every field has its own bytes:
    //
    //   0..7    magic                 8..11   format version
    //   12..15  header size           16..23  generation
    //   24..31  epoch                 32..39  payload length
    //   40..43  payload checksum      44..47  header checksum
    //   48..63  store identifier      64..71  previous generation
    //   72..79  previous digest       80..87  pruned below
    //   88..95  recorded at           96..127 reserved
    //
    // The header checksum covers the first 44 bytes, which is every field before
    // it, so a torn or edited header is detected before the payload is read.
    std::vector<std::uint8_t> file(kGenerationHeaderBytes, 0);
    std::memcpy(file.data(), kGenerationMagic, sizeof(kGenerationMagic));
    Writer fields;
    fields.u32(header.format_version);
    fields.u32(static_cast<std::uint32_t>(kGenerationHeaderBytes));
    fields.u64(header.generation);
    fields.u64(header.epoch);
    fields.u64(header.payload_length);
    fields.u32(header.payload_crc);
    std::memcpy(file.data() + 8, fields.bytes().data(), fields.bytes().size());
    std::memcpy(file.data() + 48, header.store_id.data(), header.store_id.size());
    Writer chain;
    chain.u64(header.previous_generation);
    chain.u64(header.previous_payload_digest);
    chain.u64(header.pruned_below);
    chain.i64(header.recorded_at_ns);
    std::memcpy(file.data() + 64, chain.bytes().data(), chain.bytes().size());
    const std::uint32_t header_crc = crc32_bytes(file.data(), 44);
    std::memcpy(file.data() + 44, &header_crc, sizeof(header_crc));
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
}

Status decode_header(const std::vector<std::uint8_t>& bytes, GenerationHeader& header) {
    if (bytes.size() < kGenerationHeaderBytes) {
        return Error(ErrorCode::TornTailDiscarded, "the file is shorter than a generation header")
            .with_context("bytes", std::to_string(bytes.size()));
    }
    if (std::memcmp(bytes.data(), kGenerationMagic, sizeof(kGenerationMagic)) != 0) {
        return Error(ErrorCode::StoreLayoutInvalid, "the generation magic does not match this format");
    }
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, bytes.data() + 44, sizeof(stored_crc));
    const std::uint32_t computed_crc = crc32_bytes(bytes.data(), 44);
    if (stored_crc != computed_crc) {
        return Error(ErrorCode::HeaderChecksumMismatch, "the generation header checksum does not match")
            .with_context("stored", std::to_string(stored_crc))
            .with_context("computed", std::to_string(computed_crc));
    }
    Reader reader(bytes.data() + 8, 36);
    header.format_version = reader.u32();
    const std::uint32_t header_bytes = reader.u32();
    header.generation = reader.u64();
    header.epoch = reader.u64();
    header.payload_length = reader.u64();
    header.payload_crc = reader.u32();
    if (!reader.ok() || header_bytes != kGenerationHeaderBytes) {
        return Error(ErrorCode::StoreLayoutInvalid, "the generation header size field is not the implemented size");
    }
    if (header.format_version < limits::kMinReadableStoreFormatVersion ||
        header.format_version > limits::kStoreFormatVersion) {
        return Error(ErrorCode::UnsupportedFormatVersion, "the generation format version is not supported")
            .with_context("format_version", std::to_string(header.format_version));
    }
    std::copy(bytes.begin() + 48, bytes.begin() + 64, header.store_id.begin());
    Reader chain(bytes.data() + 64, 32);
    header.previous_generation = chain.u64();
    header.previous_payload_digest = chain.u64();
    header.pruned_below = chain.u64();
    header.recorded_at_ns = chain.i64();
    if (!chain.ok()) {
        return Error(ErrorCode::InteriorCorruption, "the generation chain fields are truncated");
    }
    return ok();
}

Status decode_generation(const std::vector<std::uint8_t>& bytes, StoreState& out, GenerationHeader& header) {
    const Status header_status = decode_header(bytes, header);
    if (!header_status) {
        return header_status.error();
    }
    if (header.payload_length > limits::kMaxGenerationPayloadBytes) {
        return Error(ErrorCode::PayloadTooLarge, "the declared payload length exceeds the configured bound")
            .with_context("declared", std::to_string(header.payload_length));
    }
    const std::uint64_t total = static_cast<std::uint64_t>(kGenerationHeaderBytes) + header.payload_length;
    if (bytes.size() < total) {
        // The header is intact and the payload is short: an interrupted write.
        return Error(ErrorCode::TornTailDiscarded, "the generation payload is shorter than the header declares")
            .with_context("declared", std::to_string(total))
            .with_context("actual", std::to_string(bytes.size()));
    }
    if (bytes.size() > total) {
        return Error(ErrorCode::InteriorCorruption, "the generation file is longer than its header declares")
            .with_context("declared", std::to_string(total))
            .with_context("actual", std::to_string(bytes.size()));
    }
    const std::uint32_t computed_crc = crc32_bytes(bytes.data() + kGenerationHeaderBytes,
                                                   static_cast<std::size_t>(header.payload_length));
    if (computed_crc != header.payload_crc) {
        return Error(ErrorCode::PayloadChecksumMismatch, "the generation payload checksum does not match")
            .with_context("stored", std::to_string(header.payload_crc))
            .with_context("computed", std::to_string(computed_crc));
    }
    StoreState state;
    state.generation = GenerationSequence(header.generation);
    state.epoch = StoreEpoch(header.epoch);
    const Status status = decode_payload(bytes.data() + kGenerationHeaderBytes,
                                         static_cast<std::size_t>(header.payload_length), state);
    if (!status) {
        return status.error();
    }
    if (state.generation != GenerationSequence(header.generation) ||
        state.store_id != header.store_id ||
        state.previous_generation != GenerationSequence(header.previous_generation) ||
        state.previous_payload_digest != header.previous_payload_digest ||
        state.pruned_below != GenerationSequence(header.pruned_below)) {
        // Header and payload must agree about everything they both carry. A
        // disagreement is corruption even when both checksums pass, because it
        // means the file was assembled from two different generations.
        return Error(ErrorCode::InteriorCorruption, "the generation header and payload disagree about the store");
    }
    out = std::move(state);
    return ok();
}

std::vector<std::uint8_t> encode_current(const CurrentRecord& record) {
    Writer writer;
    writer.u32(kCurrentMagic);
    writer.u32(record.format_version);
    write_uuid(writer, record.store_id);
    writer.u64(record.generation);
    writer.u64(record.epoch);
    writer.u64(record.payload_digest);
    std::vector<std::uint8_t> bytes = writer.take();
    const std::uint32_t checksum = crc32_bytes(bytes.data(), bytes.size());
    Writer trailer;
    trailer.u32(checksum);
    const std::vector<std::uint8_t>& tail = trailer.bytes();
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}

Status decode_current(const std::vector<std::uint8_t>& bytes, CurrentRecord& out) {
    if (bytes.size() < 4) {
        return Error(ErrorCode::CommitPointMissing, "the commit point is shorter than its own checksum")
            .with_context("bytes", std::to_string(bytes.size()));
    }
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, bytes.data() + bytes.size() - 4, sizeof(stored_crc));
    const std::uint32_t computed_crc = crc32_bytes(bytes.data(), bytes.size() - 4);
    if (stored_crc != computed_crc) {
        return Error(ErrorCode::CommitPointMissing, "the commit point checksum does not match");
    }
    Reader reader(bytes.data(), bytes.size() - 4);
    if (reader.u32() != kCurrentMagic) {
        return Error(ErrorCode::CommitPointMissing, "the commit point magic does not match this format");
    }
    out.format_version = reader.u32();
    if (!reader.ok()) {
        return Error(ErrorCode::CommitPointMissing, "the commit point is truncated");
    }
    if (out.format_version != kPayloadVersion) {
        return Error(ErrorCode::UnsupportedFormatVersion, "the commit point version is not supported")
            .with_context("format_version", std::to_string(out.format_version));
    }
    if (!read_uuid(reader, out.store_id)) {
        return Error(ErrorCode::CommitPointMissing, "the commit point store identifier is truncated");
    }
    out.generation = reader.u64();
    out.epoch = reader.u64();
    out.payload_digest = reader.u64();
    if (!reader.ok() || !reader.at_end()) {
        return Error(ErrorCode::CommitPointMissing, "the commit point is truncated or carries trailing bytes");
    }
    return ok();
}

std::vector<std::uint8_t> encode_meta(const MetaRecord& record) {
    Writer writer;
    writer.u32(kMetaMagic);
    writer.u32(record.format_version);
    write_uuid(writer, record.store_id);
    writer.i64(record.created_at_ns);
    std::vector<std::uint8_t> bytes = writer.take();
    const std::uint32_t checksum = crc32_bytes(bytes.data(), bytes.size());
    Writer trailer;
    trailer.u32(checksum);
    const std::vector<std::uint8_t>& tail = trailer.bytes();
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}

Status decode_meta(const std::vector<std::uint8_t>& bytes, MetaRecord& out) {
    if (bytes.size() < 4) {
        return Error(ErrorCode::StoreLayoutInvalid, "the store meta file is shorter than its own checksum")
            .with_context("bytes", std::to_string(bytes.size()));
    }
    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, bytes.data() + bytes.size() - 4, sizeof(stored_crc));
    if (stored_crc != crc32_bytes(bytes.data(), bytes.size() - 4)) {
        return Error(ErrorCode::StoreIntegrityFailed, "the store meta checksum does not match");
    }
    Reader reader(bytes.data(), bytes.size() - 4);
    if (reader.u32() != kMetaMagic) {
        return Error(ErrorCode::StoreLayoutInvalid, "the store meta magic does not match this format");
    }
    out.format_version = reader.u32();
    if (out.format_version != kPayloadVersion) {
        return Error(ErrorCode::UnsupportedFormatVersion, "the store format version is not supported by this build")
            .with_context("format_version", std::to_string(out.format_version));
    }
    if (!read_uuid(reader, out.store_id)) {
        return Error(ErrorCode::StoreLayoutInvalid, "the store meta identifier is truncated");
    }
    out.created_at_ns = reader.i64();
    if (!reader.ok() || !reader.at_end()) {
        return Error(ErrorCode::StoreLayoutInvalid, "the store meta file is truncated or carries trailing bytes");
    }
    return ok();
}

std::string generation_file_name(GenerationSequence generation) {
    std::string digits = generation.to_string();
    std::string name = "gen-";
    name.append(20u - std::min<std::size_t>(digits.size(), 20u), '0');
    name.append(digits);
    name.append(".ahg");
    return name;
}

std::optional<GenerationSequence> parse_generation_file_name(std::string_view name) {
    if (name.size() != 4 + 20 + 4 || name.substr(0, 4) != "gen-" || name.substr(name.size() - 4) != ".ahg") {
        return std::nullopt;
    }
    // The file name is fixed width and zero padded so that a directory listing
    // sorts in generation order by name alone. That is a different grammar from
    // the canonical counter spelling, which rejects a leading zero, so the
    // padding is removed here and the remainder is then parsed canonically.
    const std::string_view digits = name.substr(4, 20);
    const std::size_t first_significant = digits.find_first_not_of('0');
    if (first_significant == std::string_view::npos) {
        // All zeros: generation zero is never published, so this is not a name
        // this format writes.
        return std::nullopt;
    }
    return GenerationSequence::parse(digits.substr(first_significant));
}

}  // namespace asset_health::detail
