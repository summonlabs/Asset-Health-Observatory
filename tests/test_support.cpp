// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <string>
#include <system_error>
#include <utility>

namespace asset_test {

using namespace asset_health;

TempDirectory::TempDirectory(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (std::string("aho-test-") + std::string(name) + "-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_, error);
}

TempDirectory::~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

ProcessHandle start_process(const std::filesystem::path& program,
                                   const std::vector<std::string>& arguments) {
    ProcessHandle handle;
    std::vector<std::string> storage;
    storage.push_back(program.string());
    for (const std::string& argument : arguments) {
        storage.push_back(argument);
    }
#if defined(_WIN32)
    // The spawn family builds the child's command line by joining the argument
    // vector with spaces and quotes nothing itself, so an argument that contains
    // a space must be quoted here or the child sees two arguments.
    for (std::string& value : storage) {
        if (value.find_first_of(" \t") == std::string::npos) {
            continue;
        }
        std::string quoted = "\"";
        for (const char character : value) {
            if (character == '"') {
                quoted.push_back('\\');
            }
            quoted.push_back(character);
        }
        quoted.push_back('"');
        value = std::move(quoted);
    }
#endif
    std::vector<const char*> pointers;
    pointers.reserve(storage.size() + 1);
    for (const std::string& value : storage) {
        pointers.push_back(value.c_str());
    }
    pointers.push_back(nullptr);
#if defined(_WIN32)
    const intptr_t started = ::_spawnv(_P_NOWAIT, program.string().c_str(), pointers.data());
    if (started == -1) {
        return handle;
    }
    handle.value = started;
#else
    const pid_t child = ::fork();
    if (child < 0) {
        return handle;
    }
    if (child == 0) {
        ::execv(program.string().c_str(), const_cast<char* const*>(pointers.data()));
        ::_exit(127);
    }
    handle.value = static_cast<std::intptr_t>(child);
#endif
    return handle;
}

int wait_process(ProcessHandle handle) {
    if (!handle.valid()) {
        return -1;
    }
#if defined(_WIN32)
    int status = -1;
    const intptr_t result = ::_cwait(&status, static_cast<intptr_t>(handle.value), 0);
    if (result == -1) {
        return -1;
    }
    return status;
#else
    int status = 0;
    if (::waitpid(static_cast<pid_t>(handle.value), &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool file_present(const std::filesystem::path& file) {
    std::error_code error;
    return std::filesystem::exists(file, error) && !error;
}

void write_text_file(const std::filesystem::path& file, const std::string& text) {
    std::ofstream stream(file, std::ios::trunc);
    stream << text;
}

std::string read_text_file(const std::filesystem::path& file) {
    std::ifstream stream(file);
    std::string text;
    std::string line;
    while (std::getline(stream, line)) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

bool wait_for_file(const std::filesystem::path& file) {
    for (std::uint64_t poll = 0; poll < kMaxPolls; ++poll) {
        if (file_present(file)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMilliseconds));
    }
    return false;
}

bool wait_for_line(const std::filesystem::path& file, const std::string& expected) {
    for (std::uint64_t poll = 0; poll < kMaxPolls; ++poll) {
        std::ifstream stream(file);
        std::string line;
        if (std::getline(stream, line) && line == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMilliseconds));
    }
    return false;
}

std::string shell_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring wide = path.wstring();
    std::wstring buffer(wide.size() + 64, L'\0');
    const DWORD written = ::GetShortPathNameW(wide.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written != 0 && written < buffer.size()) {
        buffer.resize(written);
        std::string narrow;
        narrow.reserve(buffer.size());
        for (const wchar_t character : buffer) {
            narrow.push_back(character < 128 ? static_cast<char>(character) : '?');
        }
        if (narrow.find(' ') == std::string::npos) {
            return narrow;
        }
    }
#endif
    return path.string();
}

AssetRefId asset_id(std::string_view text) { return AssetRefId::parse(text).value(); }

Instant instant(std::string_view text) { return parse_instant(text).value(); }

Builder::Builder(AssetRefId asset, SourceId source, SourceKind kind, SourceClass klass)
    : asset_(asset), source_(std::move(source)), kind_(kind), klass_(klass) {
    observed_ = instant("2026-01-01T00:00:00Z");
}

Builder& Builder::generation(AssetGeneration value) noexcept {
    generation_ = value;
    return *this;
}

Builder& Builder::epoch(std::uint64_t value) noexcept {
    epoch_ = StreamEpoch(value);
    return *this;
}

Builder& Builder::sequence(std::uint64_t value) noexcept {
    sequence_ = StreamSequence(value);
    return *this;
}

Builder& Builder::observed(Instant value) noexcept {
    observed_ = value;
    return *this;
}

Builder& Builder::received(Instant value) noexcept {
    received_ = value;
    return *this;
}

Builder& Builder::producer_version(std::string_view value) {
    producer_version_ = VersionText::parse(value);
    return *this;
}

Builder& Builder::id(EvidenceId value) noexcept {
    id_ = value;
    return *this;
}

namespace {

}  // namespace

EvidenceRecord::Parts make_parts(const AssetRefId& asset, const SourceId& source, SourceKind kind, SourceClass klass,
                                 AssetGeneration generation, StreamEpoch epoch, StreamSequence sequence,
                                 Instant observed, const std::optional<Instant>& received,
                                 const std::optional<VersionText>& producer_version,
                                 const std::optional<EvidenceId>& id) {
    EvidenceRecord::Parts parts;
    if (id.has_value()) {
        parts.id = *id;
    } else {
        parts.id = EvidenceId::generate().value();
    }
    parts.subject = EvidenceSubject::make(asset, generation).value();
    Provenance::Parts provenance;
    provenance.source = source;
    provenance.kind = kind;
    provenance.klass = klass;
    provenance.epoch = epoch;
    provenance.sequence = sequence;
    provenance.observed_at = observed;
    provenance.received_at = received.has_value() ? *received : observed;
    provenance.producer_version = producer_version;
    parts.provenance = Provenance::make(std::move(provenance)).value();
    return parts;
}

EvidenceRecord::Parts Builder::identity(std::uint64_t revision) const {
    EvidenceRecord::Parts parts =
        make_parts(asset_, source_, kind_, klass_, generation_, epoch_, sequence_, observed_, received_,
                   producer_version_, id_);
    parts.payload.kind = EvidenceKind::IdentityStatement;
    parts.payload.identity.revision = AssetRevision(revision);
    return parts;
}

EvidenceRecord::Parts Builder::lifecycle(LifecycleState state, std::optional<Instant> effective) const {
    EvidenceRecord::Parts parts =
        make_parts(asset_, source_, kind_, klass_, generation_, epoch_, sequence_, observed_, received_,
                   producer_version_, id_);
    parts.payload.kind = EvidenceKind::LifecycleStatement;
    parts.payload.lifecycle.state = state;
    parts.payload.lifecycle.effective_at = effective;
    return parts;
}

EvidenceRecord::Parts Builder::telemetry(MetricKind metric, std::int64_t milli, SampleQuality quality,
                                         bool cached) const {
    EvidenceRecord::Parts parts =
        make_parts(asset_, source_, kind_, klass_, generation_, epoch_, sequence_, observed_, received_,
                   producer_version_, id_);
    parts.payload.kind = EvidenceKind::TelemetryReading;
    parts.payload.telemetry.metric = metric;
    parts.payload.telemetry.value = Quantity::make(canonical_unit(metric), milli).value();
    parts.payload.telemetry.quality = quality;
    parts.payload.telemetry.cached = cached;
    return parts;
}

EvidenceRecord::Parts Builder::fault(std::string_view component, std::string_view code, FaultSeverity severity,
                                     FaultStatus status) const {
    EvidenceRecord::Parts parts =
        make_parts(asset_, source_, kind_, klass_, generation_, epoch_, sequence_, observed_, received_,
                   producer_version_, id_);
    parts.payload.kind = EvidenceKind::FaultStatement;
    parts.payload.fault.component = ComponentId::parse(component).value();
    parts.payload.fault.code = FaultCode::parse(code).value();
    parts.payload.fault.severity = severity;
    parts.payload.fault.status = status;
    return parts;
}

EvidenceRecord::Parts Builder::maintenance(MaintenanceKind kind, MaintenanceState state, Instant start, Instant end,
                                           MaskScope mask) const {
    EvidenceRecord::Parts parts =
        make_parts(asset_, source_, kind_, klass_, generation_, epoch_, sequence_, observed_, received_,
                   producer_version_, id_);
    parts.payload.kind = EvidenceKind::MaintenanceStatement;
    parts.payload.maintenance.kind = kind;
    parts.payload.maintenance.state = state;
    parts.payload.maintenance.window_start = start;
    parts.payload.maintenance.window_end = end;
    parts.payload.maintenance.mask = mask;
    return parts;
}

EvidenceRecord::Parts Builder::firmware(std::string_view component, std::string_view version,
                                        std::string_view baseline, FirmwareCompliance compliance) const {
    EvidenceRecord::Parts parts =
        make_parts(asset_, source_, kind_, klass_, generation_, epoch_, sequence_, observed_, received_,
                   producer_version_, id_);
    parts.payload.kind = EvidenceKind::FirmwareStatement;
    if (!component.empty()) {
        parts.payload.firmware.component = ComponentId::parse(component).value();
    }
    parts.payload.firmware.version = VersionText::parse(version).value();
    if (!baseline.empty()) {
        parts.payload.firmware.baseline = VersionText::parse(baseline).value();
    }
    parts.payload.firmware.compliance = compliance;
    return parts;
}

}  // namespace asset_test
