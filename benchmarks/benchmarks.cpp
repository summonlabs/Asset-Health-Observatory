// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Benchmarks. Every number printed is completed work: the operations counted are
// the ones that returned, and the label says whether the evidence is REAL
// measurements, SYNTHETIC input that this program generated, or UNSUPPORTED
// because no facility hardware was involved.
//
// The ingest benchmark measures the whole durable path, including the generation
// write, because that is the work an ingest actually costs. The report states
// the cost model next to the number: a commit rewrites the whole evidence set.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "asset_health/export.hpp"
#include "asset_health/evaluation.hpp"
#include "asset_health/observatory.hpp"

namespace {

using namespace asset_health;

constexpr const char* kAssetText = "12121212-3434-4565-8787-909090909090";

struct Measurement {
    std::string name;
    std::string label;
    std::uint64_t operations = 0;
    double seconds = 0.0;
};

void report(const Measurement& measurement) {
    const double per_operation =
        measurement.operations == 0 ? 0.0 : (measurement.seconds * 1.0e6) / static_cast<double>(measurement.operations);
    std::cout << measurement.name << ": " << measurement.operations << " completed operations in " << measurement.seconds
              << " s (" << per_operation << " us/op) [" << measurement.label << "]\n";
}

[[nodiscard]] EvidenceRecord::Parts telemetry_parts(const AssetRefId& asset, std::uint64_t sequence, Instant observed,
                                                    std::int64_t milli) {
    EvidenceRecord::Parts parts;
    parts.id = EvidenceId::generate().value();
    parts.subject = EvidenceSubject::make(asset, AssetGeneration(1)).value();
    Provenance::Parts provenance;
    provenance.source = SourceId::parse("bench-telemetry").value();
    provenance.kind = SourceKind::TelemetryFeed;
    provenance.klass = SourceClass::Peer;
    provenance.epoch = StreamEpoch(1);
    provenance.sequence = StreamSequence(sequence);
    provenance.observed_at = observed;
    provenance.received_at = observed;
    parts.provenance = Provenance::make(std::move(provenance)).value();
    parts.payload.kind = EvidenceKind::TelemetryReading;
    parts.payload.telemetry.metric = MetricKind::Temperature;
    parts.payload.telemetry.value = Quantity::make(Unit::Celsius, milli).value();
    parts.payload.telemetry.quality = SampleQuality::Good;
    return parts;
}

}  // namespace

int main(int argc, char** argv) {
    bool full = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--full") {
            full = true;
        } else if (argument == "--small") {
            full = false;
        }
    }
    const std::uint64_t ingest_count = full ? 2000 : 200;
    const std::uint64_t assess_count = full ? 5000 : 500;
    const std::uint64_t evaluation_count = full ? 20000 : 2000;

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aho-benchmarks";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ObservatoryOptions options;
    options.store_root = root;
    options.max_evidence_per_asset = 4096;
    auto opened = Observatory::open(options);
    if (!opened) {
        std::cerr << "benchmarks: " << opened.error().to_string() << "\n";
        return EXIT_FAILURE;
    }
    Observatory observatory = std::move(opened.value());
    const AssetRefId asset = AssetRefId::parse(kAssetText).value();
    const Instant start = *parse_instant("2026-05-01T00:00:00Z");

    Measurement ingest;
    ingest.name = "ingest-commit";
    ingest.label = "SYNTHETIC input, REAL durable commit; each commit rewrites the whole evidence set";
    {
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < ingest_count; ++index) {
            const auto observed = *start.shifted(Duration::from_seconds(static_cast<std::int64_t>(index)));
            auto outcome = observatory.ingest(telemetry_parts(asset, index + 1, observed, 40000 + static_cast<std::int64_t>(index % 500)));
            if (!outcome || !outcome.value().admitted) {
                std::cerr << "benchmarks: ingest failed at " << index << "\n";
                return EXIT_FAILURE;
            }
            ++ingest.operations;
        }
        const auto end = std::chrono::steady_clock::now();
        ingest.seconds = std::chrono::duration<double>(end - begin).count();
    }
    report(ingest);

    Measurement assess;
    assess.name = "assess-published-snapshot";
    assess.label = "SYNTHETIC input, REAL evaluation of an admitted evidence set";
    {
        const Instant at = *start.shifted(Duration::from_seconds(static_cast<std::int64_t>(ingest_count) + 1));
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < assess_count; ++index) {
            auto assessment = observatory.assess(asset, at);
            if (!assessment) {
                std::cerr << "benchmarks: assess failed\n";
                return EXIT_FAILURE;
            }
            ++assess.operations;
        }
        const auto end = std::chrono::steady_clock::now();
        assess.seconds = std::chrono::duration<double>(end - begin).count();
    }
    report(assess);

    Measurement direct;
    direct.name = "evaluate-pure-function";
    direct.label = "SYNTHETIC input, REAL evaluation of an in-memory evidence set with no store";
    {
        std::vector<EvidenceRecord> records;
        for (std::uint64_t index = 0; index < 256; ++index) {
            const auto observed = *start.shifted(Duration::from_seconds(static_cast<std::int64_t>(index)));
            const auto parts = telemetry_parts(asset, index + 1, observed, 40000 + static_cast<std::int64_t>(index % 500));
            records.push_back(EvidenceRecord::make(parts).value().with_commit_epoch(StoreEpoch(1)));
        }
        EvaluationRequest request;
        request.asset = asset;
        request.generation = AssetGeneration(1);
        request.evidence = records;
        EvaluationContext context;
        context.now = *start.shifted(Duration::from_seconds(300));
        context.store_epoch = StoreEpoch(1);
        const HealthPolicy& policy = HealthPolicy::standard();
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < evaluation_count; ++index) {
            const HealthAssessment assessment = evaluate(request, policy, context);
            if (assessment.findings.empty()) {
                std::cerr << "benchmarks: evaluation produced no findings for 256 readings\n";
                return EXIT_FAILURE;
            }
            ++direct.operations;
        }
        const auto end = std::chrono::steady_clock::now();
        direct.seconds = std::chrono::duration<double>(end - begin).count();
    }
    report(direct);

    Measurement reopen;
    reopen.name = "reopen-and-recover";
    reopen.label = "SYNTHETIC input, REAL durable reopen and recovery";
    {
        observatory.close();
        const auto begin = std::chrono::steady_clock::now();
        auto again = Observatory::open(options);
        if (!again) {
            std::cerr << "benchmarks: reopen failed: " << again.error().to_string() << "\n";
            return EXIT_FAILURE;
        }
        ++reopen.operations;
        const auto end = std::chrono::steady_clock::now();
        reopen.seconds = std::chrono::duration<double>(end - begin).count();
        const RecoveryReport& recovery = again.value().recovery();
        std::cout << "reopen recovered " << recovery.evidence_records << " evidence records at generation "
                  << recovery.recovered_generation.to_string() << " and demoted "
                  << recovery.dynamic_records_demoted << " dynamic records to non-fresh\n";
        again.value().close();
    }
    report(reopen);

    std::filesystem::remove_all(root, error);
    std::cout << "UNSUPPORTED: no facility hardware, BMS, DCIM, electrical or cooling instrument was involved; all "
                 "input above was generated by this program.\n";
    return EXIT_SUCCESS;
}
