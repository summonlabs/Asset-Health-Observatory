// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Shows the offline side of the runtime: a store is written by one process,
// closed, reopened by another open call, and audited. This is the shape an
// operator tool uses when it wants an answer without holding the writer lock.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "asset_health/export.hpp"
#include "asset_health/observatory.hpp"

namespace {

using namespace asset_health;

constexpr const char* kAssetText = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

}  // namespace

int main() {
    using namespace asset_health;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aho-example-offline";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const Instant observed = *parse_instant("2026-04-01T12:00:00Z");
    ObservatoryOptions options;
    options.store_root = root;

    {
        auto opened = Observatory::open(options);
        if (!opened) {
            std::cerr << "example_offline: " << opened.error().to_string() << "\n";
            return EXIT_FAILURE;
        }
        Observatory observatory = std::move(opened.value());
        EvidenceRecord::Parts parts;
        parts.id = EvidenceId::generate().value();
        parts.subject = EvidenceSubject::make(AssetRefId::parse(kAssetText).value(), AssetGeneration(1)).value();
        Provenance::Parts provenance;
        provenance.source = SourceId::parse("lifecycle-b").value();
        provenance.kind = SourceKind::HardwareLifecycle;
        provenance.klass = SourceClass::Authoritative;
        provenance.epoch = StreamEpoch(1);
        provenance.sequence = StreamSequence(1);
        provenance.observed_at = observed;
        provenance.received_at = observed;
        parts.provenance = Provenance::make(std::move(provenance)).value();
        parts.payload.kind = EvidenceKind::LifecycleStatement;
        parts.payload.lifecycle.state = LifecycleState::Active;
        if (!observatory.ingest(parts)) {
            std::cerr << "example_offline: ingest failed\n";
            return EXIT_FAILURE;
        }
        if (!observatory.assess_and_publish(AssetRefId::parse(kAssetText).value(),
                                            *observed.shifted(Duration::from_seconds(30)))) {
            std::cerr << "example_offline: publish failed\n";
            return EXIT_FAILURE;
        }
        observatory.close();
    }

    // Read-only open: no lock is taken, nothing is created, and exactly one
    // committed generation is observed.
    ObservatoryOptions read_only = options;
    read_only.read_only = true;
    read_only.create_if_missing = false;
    auto reopened = Observatory::open(read_only);
    if (!reopened) {
        std::cerr << "example_offline: read-only open failed: " << reopened.error().to_string() << "\n";
        return EXIT_FAILURE;
    }
    Observatory offline = std::move(reopened.value());
    auto history = offline.history(AssetRefId::parse(kAssetText).value());
    if (!history) {
        std::cerr << "example_offline: history failed\n";
        return EXIT_FAILURE;
    }
    if (history.value().entries.empty()) {
        std::cerr << "example_offline: expected at least one published entry\n";
        return EXIT_FAILURE;
    }
    std::cout << history.value().to_string();

    auto audit = offline.verify();
    if (!audit) {
        std::cerr << "example_offline: verify failed\n";
        return EXIT_FAILURE;
    }
    if (!audit.value().ok) {
        std::cerr << "example_offline: audit reported problems:\n" << audit.value().to_string() << "\n";
        return EXIT_FAILURE;
    }
    offline.close();

    // A writable reopen publishes a strictly greater epoch and reports that it
    // recovered durable state.
    auto writable = Observatory::open(options);
    if (!writable) {
        std::cerr << "example_offline: reopen failed\n";
        return EXIT_FAILURE;
    }
    const RecoveryReport& report = writable.value().recovery();
    if (report.current_epoch <= report.previous_epoch) {
        std::cerr << "example_offline: the reopened epoch did not advance\n";
        return EXIT_FAILURE;
    }
    std::cout << "recovery: " << report.to_string() << "\n";
    writable.value().close();

    std::filesystem::remove_all(root, error);
    std::cout << "example_offline: ok\n";
    return EXIT_SUCCESS;
}
