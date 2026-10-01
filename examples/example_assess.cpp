// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Assembles evidence for one asset, asks the observatory what health state the
// evidence supports, and prints the explanation. Also a ctest case: it asserts
// the outcome rather than only printing it.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "asset_health/export.hpp"
#include "asset_health/observatory.hpp"

namespace {

using namespace asset_health;

constexpr const char* kAssetText = "11111111-2222-4333-8444-555555555555";
constexpr const char* kNoiseSourceText = "noise-feed";

int fail(const std::string& message) {
    std::cerr << "example_assess: " << message << "\n";
    return EXIT_FAILURE;
}

[[nodiscard]] Instant base_instant() { return *parse_instant("2026-03-01T00:00:00Z"); }

EvidenceRecord::Parts make_parts(EvidencePayload payload, SourceKind kind, SourceClass klass, const char* source,
                                 std::uint64_t sequence, Instant observed) {
    EvidenceRecord::Parts parts;
    parts.id = EvidenceId::generate().value();
    const auto asset = AssetRefId::parse(kAssetText).value();
    parts.subject = EvidenceSubject::make(asset, AssetGeneration(1)).value();
    Provenance::Parts provenance;
    provenance.source = SourceId::parse(source).value();
    provenance.kind = kind;
    provenance.klass = klass;
    provenance.epoch = StreamEpoch(1);
    provenance.sequence = StreamSequence(sequence);
    provenance.observed_at = observed;
    provenance.received_at = observed;
    parts.provenance = Provenance::make(std::move(provenance)).value();
    parts.payload = payload;
    return parts;
}

}  // namespace

int main() {
    using namespace asset_health;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aho-example-assess";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    ObservatoryOptions options;
    options.store_root = root;
    options.ingest_workers = 0;
    auto opened = Observatory::open(options);
    if (!opened) {
        return fail("opening the observatory failed: " + opened.error().to_string());
    }
    Observatory observatory = std::move(opened.value());

    const Instant observed = base_instant();

    EvidencePayload identity;
    identity.kind = EvidenceKind::IdentityStatement;
    identity.identity.revision = AssetRevision(4);
    if (!observatory.ingest(make_parts(identity, SourceKind::AssetRegistry, SourceClass::Authoritative,
                                       "asset-registry-a", 1, observed))) {
        return fail("identity ingest failed");
    }

    EvidencePayload lifecycle;
    lifecycle.kind = EvidenceKind::LifecycleStatement;
    lifecycle.lifecycle.state = LifecycleState::Active;
    lifecycle.lifecycle.effective_at = observed;
    if (!observatory.ingest(make_parts(lifecycle, SourceKind::HardwareLifecycle, SourceClass::Authoritative,
                                       "lifecycle-a", 1, observed))) {
        return fail("lifecycle ingest failed");
    }

    EvidencePayload temperature;
    temperature.kind = EvidenceKind::TelemetryReading;
    temperature.telemetry.metric = MetricKind::Temperature;
    temperature.telemetry.value = Quantity::make(Unit::Celsius, 82000).value();
    temperature.telemetry.quality = SampleQuality::Good;
    if (!observatory.ingest(make_parts(temperature, SourceKind::TelemetryFeed, SourceClass::Peer, kNoiseSourceText, 1,
                                       observed))) {
        return fail("temperature ingest failed");
    }

    EvidencePayload fault;
    fault.kind = EvidenceKind::FaultStatement;
    fault.fault.component = ComponentId::parse("pump-1").value();
    fault.fault.code = FaultCode::parse("coolant-flow-low").value();
    fault.fault.severity = FaultSeverity::Major;
    fault.fault.status = FaultStatus::Active;
    // Observed nine minutes after the reading, which is inside the dynamic
    // freshness window, so the fault still counts.
    const auto fault_parts = make_parts(fault, SourceKind::FaultFeed, SourceClass::Peer, "faults-a", 1,
                                        *observed.shifted(Duration::from_minutes(9)));
    if (!observatory.ingest(fault_parts)) {
        return fail("fault ingest failed");
    }

    auto assessment = observatory.assess(AssetRefId::parse(kAssetText).value(),
                                         *observed.shifted(Duration::from_minutes(10)));
    if (!assessment) {
        return fail("assessment failed: " + assessment.error().to_string());
    }
    std::cout << assessment.value().explain();

    if (assessment.value().state != HealthState::Critical) {
        return fail("expected the active major fault to support a critical state, got " +
                    std::string(to_string(assessment.value().state)));
    }
    if (!assessment.value().flags.contains(AssessmentFlag::UnknownFirmware)) {
        return fail("expected the unknown firmware state to be reported");
    }

    // Missing telemetry is unknown, not healthy: a second asset with an identity
    // and a lifecycle statement but no temperature reading cannot be called well.
    const auto second = AssetRefId::parse("99999999-8888-4777-8666-555555555555").value();
    EvidenceRecord::Parts identity_parts = make_parts(identity, SourceKind::AssetRegistry,
                                                      SourceClass::Authoritative, "asset-registry-a", 2, observed);
    identity_parts.subject = EvidenceSubject::make(second, AssetGeneration(1)).value();
    identity_parts.id = EvidenceId::generate().value();
    if (!observatory.ingest(identity_parts)) {
        return fail("second identity ingest failed");
    }
    EvidenceRecord::Parts lifecycle_parts = make_parts(lifecycle, SourceKind::HardwareLifecycle,
                                                       SourceClass::Authoritative, "lifecycle-a", 2, observed);
    lifecycle_parts.subject = EvidenceSubject::make(second, AssetGeneration(1)).value();
    lifecycle_parts.id = EvidenceId::generate().value();
    if (!observatory.ingest(lifecycle_parts)) {
        return fail("second lifecycle ingest failed");
    }
    auto second_assessment = observatory.assess(second, *observed.shifted(Duration::from_minutes(10)));
    if (!second_assessment) {
        return fail("second assessment failed");
    }
    if (second_assessment.value().state != HealthState::Unknown) {
        return fail("expected unknown for an asset with no temperature reading, got " +
                    std::string(to_string(second_assessment.value().state)));
    }
    if (!second_assessment.value().flags.contains(AssessmentFlag::MissingRequiredEvidence)) {
        return fail("expected the missing required evidence flag");
    }

    observatory.close();
    std::filesystem::remove_all(root, error);
    std::cout << "example_assess: ok\n";
    return EXIT_SUCCESS;
}
