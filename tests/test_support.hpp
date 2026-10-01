// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Test support: a temporary store directory, evidence builders that keep the
// cases readable, and a seeded generator so a randomised case is reproducible
// from its seed.

#ifndef ASSET_HEALTH_TEST_SUPPORT_HPP
#define ASSET_HEALTH_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "asset_health/observatory.hpp"

namespace asset_test {

/// Poll interval and bound used by the process helpers. The wait is bounded so
/// that a caller reports a failure instead of hanging; it is not a test timeout
/// and it never turns an unfinished case into a pass.
inline constexpr std::uint64_t kPollMilliseconds = 20;
inline constexpr std::uint64_t kMaxPolls = 3000;  // 3000 * 20 ms

/// A directory under the system temporary directory that is removed when the
/// object dies. Named so that a leftover from a crashed run is identifiable.
class TempDirectory {
public:
    explicit TempDirectory(std::string_view name);
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// Deterministic pseudo-random generator (xorshift64*). Every randomised case
/// prints its seed on failure, and re-running with that seed reproduces exactly
/// the same sequence.
class Random {
public:
    explicit Random(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1Dull;
    }

    /// A value in [0, bound).
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept { return bound == 0 ? 0 : next() % bound; }

    [[nodiscard]] std::int64_t range(std::int64_t low, std::int64_t high) noexcept {
        if (high <= low) {
            return low;
        }
        return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low)));
    }

    [[nodiscard]] std::uint64_t seed() const noexcept { return state_; }

private:
    std::uint64_t state_;
};

/// Builds evidence statement parts with every provenance field explicit.
class Builder {
public:
    Builder(asset_health::AssetRefId asset, asset_health::SourceId source, asset_health::SourceKind kind,
            asset_health::SourceClass klass);

    Builder& generation(asset_health::AssetGeneration value) noexcept;
    Builder& epoch(std::uint64_t value) noexcept;
    Builder& sequence(std::uint64_t value) noexcept;
    Builder& observed(asset_health::Instant value) noexcept;
    Builder& received(asset_health::Instant value) noexcept;
    Builder& producer_version(std::string_view value);
    Builder& id(asset_health::EvidenceId value) noexcept;

    [[nodiscard]] asset_health::EvidenceRecord::Parts identity(std::uint64_t revision) const;
    [[nodiscard]] asset_health::EvidenceRecord::Parts lifecycle(asset_health::LifecycleState state,
                                                                std::optional<asset_health::Instant> effective) const;
    [[nodiscard]] asset_health::EvidenceRecord::Parts telemetry(asset_health::MetricKind metric, std::int64_t milli,
                                                                asset_health::SampleQuality quality,
                                                                bool cached = false) const;
    [[nodiscard]] asset_health::EvidenceRecord::Parts fault(std::string_view component, std::string_view code,
                                                            asset_health::FaultSeverity severity,
                                                            asset_health::FaultStatus status) const;
    [[nodiscard]] asset_health::EvidenceRecord::Parts maintenance(asset_health::MaintenanceKind kind,
                                                                  asset_health::MaintenanceState state,
                                                                  asset_health::Instant start,
                                                                  asset_health::Instant end,
                                                                  asset_health::MaskScope mask) const;
    [[nodiscard]] asset_health::EvidenceRecord::Parts firmware(std::string_view component, std::string_view version,
                                                               std::string_view baseline,
                                                               asset_health::FirmwareCompliance compliance) const;

private:
    asset_health::AssetRefId asset_;
    asset_health::SourceId source_;
    asset_health::SourceKind kind_;
    asset_health::SourceClass klass_;
    asset_health::AssetGeneration generation_{1};
    asset_health::StreamEpoch epoch_{1};
    asset_health::StreamSequence sequence_{1};
    asset_health::Instant observed_{};
    std::optional<asset_health::Instant> received_;
    std::optional<asset_health::VersionText> producer_version_;
    std::optional<asset_health::EvidenceId> id_;
};

/// A handle on a program running as a separate operating-system process.
struct ProcessHandle {
    std::intptr_t value = -1;
    [[nodiscard]] bool valid() const noexcept { return value >= 0; }
};

/// Starts a program directly, without a command interpreter, so a path
/// containing a space needs no quoting and no shell is trusted with the
/// arguments. Throws nothing: an unusable handle is reported.
[[nodiscard]] ProcessHandle start_process(const std::filesystem::path& program,
                                          const std::vector<std::string>& arguments);

/// Waits for a process and returns its exit status, or -1 when the process could
/// not be started or did not run to completion.
[[nodiscard]] int wait_process(ProcessHandle handle);

/// True when a file exists.
[[nodiscard]] bool file_present(const std::filesystem::path& file);

/// Writes text to a file, replacing it.
void write_text_file(const std::filesystem::path& file, const std::string& text);

/// Reads a whole file as text.
[[nodiscard]] std::string read_text_file(const std::filesystem::path& file);

/// Polls until a file exists. The wait is bounded and exhausting it is reported
/// as false, so a caller fails the case rather than turning a hang into a pass.
[[nodiscard]] bool wait_for_file(const std::filesystem::path& file);

/// Polls until a file starts with the expected line.
[[nodiscard]] bool wait_for_line(const std::filesystem::path& file, const std::string& expected);

/// A path the command interpreter can be given without quoting: the short (8.3)
/// form on Windows when the file system provides one, and the path unchanged
/// otherwise. Used by the suites that run a real program as a separate process,
/// because a program path containing a space is otherwise at the mercy of the
/// interpreter's quote stripping.
[[nodiscard]] std::string shell_path(const std::filesystem::path& path);

/// A canonical identifier literal for a test, parsed once.
[[nodiscard]] asset_health::AssetRefId asset_id(std::string_view text);
[[nodiscard]] asset_health::Instant instant(std::string_view text);

/// The canonical test asset and a second one, as text.
inline constexpr const char* kAssetA = "11111111-1111-4111-8111-111111111111";
inline constexpr const char* kAssetB = "22222222-2222-4222-8222-222222222222";

}  // namespace asset_test

#endif  // ASSET_HEALTH_TEST_SUPPORT_HPP
