// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A real program the process suites run as a separate operating-system process.
//
// Modes:
//   write <store> <count>          ingest records and close cleanly
//   crash <store> <count>          ingest records and terminate without closing
//   torn  <store> <count>          ingest, shorten the newest generation, crash
//   hold  <store> <marker> <release>
//                                  take the write lock, announce it, wait for the
//                                  release marker, then exit
//
// The wait in hold mode is bounded: exhausting the bound exits with status 3 so
// that the suite reports a failure. It is not a test timeout and it never turns a
// hang into a pass -- nothing here sleeps on a schedule, it polls for a file the
// parent creates.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "asset_health/observatory.hpp"
#include "store_format.hpp"

namespace {

using namespace asset_health;

constexpr const char* kAssetText = "33333333-3333-4333-8333-333333333333";

[[nodiscard]] Instant base_instant() { return *parse_instant("2026-06-01T00:00:00Z"); }

void write_marker(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::trunc);
    stream << text << "\n";
}

[[nodiscard]] bool marker_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

[[nodiscard]] int ingest_records(Observatory& observatory, std::uint64_t count) {
    const AssetRefId asset = AssetRefId::parse(kAssetText).value();
    for (std::uint64_t index = 0; index < count; ++index) {
        EvidenceRecord::Parts parts;
        parts.id = EvidenceId::generate().value();
        parts.subject = EvidenceSubject::make(asset, AssetGeneration(1)).value();
        Provenance::Parts provenance;
        provenance.source = *SourceId::parse("helper-feed");
        provenance.kind = SourceKind::TelemetryFeed;
        provenance.klass = SourceClass::Peer;
        provenance.epoch = StreamEpoch(1);
        provenance.sequence = StreamSequence(index + 1);
        const Instant observed = *base_instant().shifted(Duration::from_seconds(static_cast<std::int64_t>(index)));
        provenance.observed_at = observed;
        provenance.received_at = observed;
        parts.provenance = Provenance::make(std::move(provenance)).value();
        parts.payload.kind = EvidenceKind::TelemetryReading;
        parts.payload.telemetry.metric = MetricKind::Temperature;
        parts.payload.telemetry.value = Quantity::make(Unit::Celsius, 40000 + static_cast<std::int64_t>(index)).value();
        parts.payload.telemetry.quality = SampleQuality::Good;
        const auto outcome = observatory.ingest(parts);
        if (!outcome || !outcome.value().admitted) {
            std::cerr << "helper: ingest failed at " << index << "\n";
            return 2;
        }
    }
    return 0;
}

[[nodiscard]] int newest_generation_file(const std::filesystem::path& store, std::filesystem::path& out) {
    std::error_code error;
    GenerationSequence highest{};
    for (const auto& entry : std::filesystem::directory_iterator(store / "generations", error)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto parsed = detail::parse_generation_file_name(entry.path().filename().string());
        if (parsed.has_value() && *parsed > highest) {
            highest = *parsed;
        }
    }
    if (highest.is_zero()) {
        return 3;
    }
    out = store / "generations" / detail::generation_file_name(highest);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "helper: mode and store path are required\n";
        return 1;
    }
    const std::string mode = argv[1];
    const std::filesystem::path store = argv[2];

    ObservatoryOptions options;
    options.store_root = store;
    options.ingest_workers = 0;

    if (mode == "hold") {
        if (argc < 5) {
            std::cerr << "helper: hold needs a marker and a release path\n";
            return 1;
        }
        const std::filesystem::path marker = argv[3];
        const std::filesystem::path release = argv[4];
        auto opened = Observatory::open(options);
        if (!opened) {
            write_marker(marker, "failed:" + std::string(error_code_name(opened.error().code())));
            return 2;
        }
        write_marker(marker, "holding");
        // A bounded poll. Exhausting it is a failure the suite reports, not a
        // pass, and it is not a timeout applied to the suite as a whole.
        constexpr std::uint64_t kMaxPolls = 60000;
        for (std::uint64_t poll = 0; poll < kMaxPolls; ++poll) {
            if (marker_exists(release)) {
                opened.value().close();
                write_marker(marker, "released");
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cerr << "helper: the parent never released the lock\n";
        return 3;
    }

    const std::uint64_t count = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;

    if (mode == "write") {
        auto opened = Observatory::open(options);
        if (!opened) {
            std::cerr << "helper: " << opened.error().to_string() << "\n";
            return 2;
        }
        const int status = ingest_records(opened.value(), count);
        if (status != 0) {
            return status;
        }
        opened.value().close();
        return 0;
    }

    if (mode == "race") {
        // Both racers try; each writes its own result file, so the parent can
        // tell a lock refusal from a defect without a shell.
        if (argc < 5) {
            std::cerr << "helper: race needs a result path\n";
            return 1;
        }
        const std::filesystem::path result = argv[4];
        auto opened = Observatory::open(options);
        if (!opened) {
            write_marker(result, std::string("refused ") + std::string(error_code_name(opened.error().code())));
            return 0;
        }
        const int status = ingest_records(opened.value(), count);
        if (status != 0) {
            write_marker(result, "failed");
            return status;
        }
        opened.value().close();
        write_marker(result, "wrote " + std::to_string(count));
        return 0;
    }

    if (mode == "crash") {
        auto opened = Observatory::open(options);
        if (!opened) {
            return 2;
        }
        const int status = ingest_records(opened.value(), count);
        if (status != 0) {
            return status;
        }
        // Terminate without closing: no clean marker is written, so the next open
        // sees a session that died.
        std::_Exit(0);
    }

    if (mode == "torn") {
        auto opened = Observatory::open(options);
        if (!opened) {
            return 2;
        }
        const int status = ingest_records(opened.value(), count);
        if (status != 0) {
            return status;
        }
        std::filesystem::path newest;
        if (newest_generation_file(store, newest) != 0) {
            return 3;
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(newest, error);
        if (error || size < 32) {
            return 3;
        }
        std::filesystem::resize_file(newest, size - 8, error);
        if (error) {
            return 3;
        }
        std::_Exit(0);
    }

    std::cerr << "helper: unknown mode " << mode << "\n";
    return 1;
}
