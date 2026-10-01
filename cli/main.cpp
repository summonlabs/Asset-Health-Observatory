// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// asset-health: the command line front end of the Asset Health Observatory.
//
// The tool never repairs an input and never guesses a missing field. A line it
// cannot parse is a usage error; a statement it cannot admit is reported with
// the reason the observatory gave, and the refusal is durable.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "asset_health/export.hpp"
#include "asset_health/observatory.hpp"

namespace {

using namespace asset_health;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;
constexpr int kExitIntegrity = 3;

constexpr const char* kVersion = "1.0.0";

void print_usage(std::ostream& out) {
    out << "asset-health " << kVersion << " - evidence-bound facility asset health observation\n"
        << "\n"
        << "usage: asset-health <command> [options]\n"
        << "\n"
        << "commands:\n"
        << "  ingest     admit evidence documents (one statement per line) and commit them\n"
        << "  assess     assess one asset and print the state, findings and justification\n"
        << "  history    print the published assessment history of one asset\n"
        << "  evidence   print the admitted statements about one asset\n"
        << "  refusals   print the statements that were refused for one asset\n"
        << "  sources    print the producer streams the store remembers\n"
        << "  stats      print store statistics\n"
        << "  verify     audit every file in the store without changing anything\n"
        << "  policy     print the health policy and its fingerprint\n"
        << "  help       print this text\n"
        << "\n"
        << "common options:\n"
        << "  --store <dir>     store directory (default: ./asset-health-store)\n"
        << "  --json            print canonical JSON instead of text\n"
        << "\n"
        << "ingest options:\n"
        << "  --file <path>     read the statements from a file (default: standard input)\n"
        << "  --no-create       refuse to create a store that does not exist\n"
        << "\n"
        << "assess options:\n"
        << "  --asset <id>      asset identity, canonical lowercase hyphenated form\n"
        << "  --at <instant>    evaluation instant, for example 2026-01-01T00:00:00Z\n"
        << "  --publish         publish the answer into durable history\n"
        << "\n"
        << "statement syntax (one per line, fields separated by spaces, kind first):\n"
        << "  identity    asset=<id> gen=<n> revision=<n> source=<id> source-kind=asset-registry \\\n"
        << "              class=authoritative epoch=<n> seq=<n> observed=<instant> [received=<instant>]\n"
        << "  lifecycle   asset=<id> gen=<n> state=<ordered|staged|installed|commissioning|active|degraded|\\\n"
        << "              maintenance|quarantined|retiring|retired|replaced|removed> source=<id> \\\n"
        << "              source-kind=hardware-lifecycle class=authoritative epoch=<n> seq=<n> observed=<instant>\n"
        << "  telemetry   asset=<id> gen=<n> metric=<name> value=<decimal><unit> quality=<good|uncertain|\\\n"
        << "              substituted|bad> source=<id> source-kind=telemetry-feed class=<peer|synthetic> \\\n"
        << "              epoch=<n> seq=<n> observed=<instant>\n"
        << "  fault       asset=<id> gen=<n> component=<id> code=<token> severity=<info|warning|minor|major|\\\n"
        << "              critical> status=<active|cleared> source=<id> source-kind=fault-feed class=peer \\\n"
        << "              epoch=<n> seq=<n> observed=<instant>\n"
        << "  maintenance asset=<id> gen=<n> kind=<planned|corrective|inspection|calibration|emergency> \\\n"
        << "              state=<scheduled|active|completed|cancelled> start=<instant> end=<instant> \\\n"
        << "              mask=<none|telemetry|faults|telemetry-and-faults> source=<id> \\\n"
        << "              source-kind=maintenance-coordinator class=authoritative epoch=<n> seq=<n> observed=<instant>\n"
        << "  firmware    asset=<id> gen=<n> [component=<id>] version=<label> \\\n"
        << "              compliance=<unknown|matches|behind|ahead> [baseline=<label>] source=<id> \\\n"
        << "              source-kind=firmware-baseline class=authoritative epoch=<n> seq=<n> observed=<instant>\n"
        << "\n"
        << "exit codes: 0 success, 1 usage error, 2 operation refused or failed, 3 store integrity failure\n";
}

struct Arguments {
    std::string command;
    std::filesystem::path store = "asset-health-store";
    std::string asset;
    std::string at;
    std::string file;
    bool json = false;
    bool publish = false;
    bool create = true;
    bool store_given = false;
};

[[nodiscard]] bool parse_arguments(int argc, char** argv, Arguments& arguments, std::string& problem) {
    if (argc < 2) {
        problem = "a command is required";
        return false;
    }
    arguments.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string_view token = argv[index];
        auto value_of = [&](std::string& target) {
            if (index + 1 >= argc) {
                problem = "option " + std::string(token) + " needs a value";
                return false;
            }
            target = argv[++index];
            return true;
        };
        if (token == "--store") {
            std::string value;
            if (!value_of(value)) {
                return false;
            }
            arguments.store = value;
            arguments.store_given = true;
        } else if (token == "--asset") {
            if (!value_of(arguments.asset)) {
                return false;
            }
        } else if (token == "--at") {
            if (!value_of(arguments.at)) {
                return false;
            }
        } else if (token == "--file") {
            if (!value_of(arguments.file)) {
                return false;
            }
        } else if (token == "--json") {
            arguments.json = true;
        } else if (token == "--publish") {
            arguments.publish = true;
        } else if (token == "--no-create") {
            arguments.create = false;
        } else {
            problem = "unknown option " + std::string(token);
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::map<std::string, std::string> parse_fields(const std::string& line, std::string& problem) {
    std::map<std::string, std::string> fields;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos || equals == 0 || equals + 1 >= token.size()) {
            problem = "field '" + token + "' is not written as name=value";
            return {};
        }
        const std::string name = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (fields.find(name) != fields.end()) {
            problem = "field '" + name + "' is given twice";
            return {};
        }
        fields[name] = value;
    }
    return fields;
}

[[nodiscard]] std::optional<std::string> field(const std::map<std::string, std::string>& fields, const char* name) {
    const auto entry = fields.find(name);
    if (entry == fields.end()) {
        return std::nullopt;
    }
    return entry->second;
}

[[nodiscard]] std::string required(const std::map<std::string, std::string>& fields, const char* name,
                                   std::string& problem) {
    const auto value = field(fields, name);
    if (!value.has_value()) {
        problem = std::string("field '") + name + "' is required";
        return {};
    }
    return *value;
}

[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
    }
    out = value;
    return true;
}

/// Builds one statement from a parsed line.
[[nodiscard]] bool build_record(const std::string& line, EvidenceRecord::Parts& parts, std::string& problem) {
    std::istringstream stream(line);
    std::string kind_text;
    stream >> kind_text;
    const std::string remainder = line.substr(std::min(line.size(), line.find(kind_text) + kind_text.size()));
    const auto fields = parse_fields(remainder, problem);
    if (!problem.empty()) {
        return false;
    }

    const std::string asset_text = required(fields, "asset", problem);
    if (!problem.empty()) {
        return false;
    }
    const auto asset = AssetRefId::parse(asset_text);
    if (!asset.has_value()) {
        problem = "asset '" + asset_text + "' is not a canonical lowercase hyphenated identifier";
        return false;
    }
    std::uint64_t generation = 0;
    if (!parse_u64(required(fields, "gen", problem), generation) || generation == 0) {
        problem = "gen must be a positive decimal number";
        return false;
    }
    const std::string source_text = required(fields, "source", problem);
    const std::string source_kind_text = required(fields, "source-kind", problem);
    const std::string class_text = required(fields, "class", problem);
    const std::string epoch_text = required(fields, "epoch", problem);
    const std::string sequence_text = required(fields, "seq", problem);
    const std::string observed_text = required(fields, "observed", problem);
    if (!problem.empty()) {
        return false;
    }
    const auto source = SourceId::parse(source_text);
    const auto source_kind = parse_source_kind(source_kind_text);
    const auto klass = parse_source_class(class_text);
    const auto observed = parse_instant(observed_text);
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
    if (!source.has_value()) {
        problem = "source '" + source_text + "' is not a canonical token";
        return false;
    }
    if (!source_kind.has_value()) {
        problem = "source-kind '" + source_kind_text + "' is not a canonical source kind";
        return false;
    }
    if (!klass.has_value()) {
        problem = "class '" + class_text + "' is not a canonical source class";
        return false;
    }
    if (!observed.has_value()) {
        problem = "observed '" + observed_text + "' is not a canonical UTC instant";
        return false;
    }
    if (!parse_u64(epoch_text, epoch) || !parse_u64(sequence_text, sequence) || sequence == 0) {
        problem = "epoch must be a decimal number and seq a positive decimal number";
        return false;
    }
    Instant received = *observed;
    if (const auto received_text = field(fields, "received"); received_text.has_value()) {
        const auto parsed = parse_instant(*received_text);
        if (!parsed.has_value()) {
            problem = "received '" + *received_text + "' is not a canonical UTC instant";
            return false;
        }
        received = *parsed;
    }

    Provenance::Parts provenance;
    provenance.source = *source;
    provenance.kind = *source_kind;
    provenance.klass = *klass;
    provenance.epoch = StreamEpoch(epoch);
    provenance.sequence = StreamSequence(sequence);
    provenance.observed_at = *observed;
    provenance.received_at = received;
    const auto built_provenance = Provenance::make(std::move(provenance));
    if (!built_provenance.has_value()) {
        problem = "the provenance is not well formed: a statement needs a non-empty source, an ingestible source "
                  "kind, a positive sequence and a received instant at or after the observed instant";
        return false;
    }

    EvidencePayload payload;
    if (kind_text == "identity") {
        payload.kind = EvidenceKind::IdentityStatement;
        std::uint64_t revision = 0;
        if (!parse_u64(required(fields, "revision", problem), revision) || revision == 0) {
            problem = "revision must be a positive decimal number";
            return false;
        }
        payload.identity.revision = AssetRevision(revision);
        if (const auto serial = field(fields, "serial"); serial.has_value()) {
            const auto text = DetailText::parse(*serial, 128);
            if (!text.has_value()) {
                problem = "serial is not printable text of at most 128 bytes";
                return false;
            }
            payload.identity.serial = *text;
        }
    } else if (kind_text == "lifecycle") {
        payload.kind = EvidenceKind::LifecycleStatement;
        const std::string state_text = required(fields, "state", problem);
        const auto state = parse_lifecycle_state(state_text);
        if (!state.has_value()) {
            problem = "state '" + state_text + "' is not a canonical lifecycle state";
            return false;
        }
        payload.lifecycle.state = *state;
        if (const auto effective = field(fields, "effective"); effective.has_value()) {
            const auto parsed = parse_instant(*effective);
            if (!parsed.has_value()) {
                problem = "effective is not a canonical UTC instant";
                return false;
            }
            payload.lifecycle.effective_at = *parsed;
        }
    } else if (kind_text == "telemetry") {
        payload.kind = EvidenceKind::TelemetryReading;
        const std::string metric_text = required(fields, "metric", problem);
        const std::string value_text = required(fields, "value", problem);
        const std::string quality_text = required(fields, "quality", problem);
        if (!problem.empty()) {
            return false;
        }
        const auto metric = parse_metric(metric_text);
        const auto value = Quantity::parse(value_text);
        const auto quality = parse_sample_quality(quality_text);
        if (!metric.has_value()) {
            problem = "metric '" + metric_text + "' is not a canonical metric name";
            return false;
        }
        if (!value.has_value()) {
            problem = "value '" + value_text + "' is not a decimal with a canonical unit and at most three "
                      "digits of precision";
            return false;
        }
        if (!quality.has_value()) {
            problem = "quality '" + quality_text + "' is not a canonical sample quality";
            return false;
        }
        payload.telemetry.metric = *metric;
        payload.telemetry.value = *value;
        payload.telemetry.quality = *quality;
        payload.telemetry.cached = field(fields, "cached").value_or("false") == "true";
    } else if (kind_text == "fault") {
        payload.kind = EvidenceKind::FaultStatement;
        const std::string component_text = required(fields, "component", problem);
        const std::string code_text = required(fields, "code", problem);
        const std::string severity_text = required(fields, "severity", problem);
        const std::string status_text = required(fields, "status", problem);
        if (!problem.empty()) {
            return false;
        }
        const auto component = ComponentId::parse(component_text);
        const auto code = FaultCode::parse(code_text);
        const auto severity = parse_fault_severity(severity_text);
        const auto status = parse_fault_status(status_text);
        if (!component.has_value()) {
            problem = "component '" + component_text + "' is not a canonical token";
            return false;
        }
        if (!code.has_value()) {
            problem = "code '" + code_text + "' is not a canonical token";
            return false;
        }
        if (!severity.has_value()) {
            problem = "severity '" + severity_text + "' is not a canonical fault severity";
            return false;
        }
        if (!status.has_value()) {
            problem = "status '" + status_text + "' is not a canonical fault status";
            return false;
        }
        payload.fault.component = *component;
        payload.fault.code = *code;
        payload.fault.severity = *severity;
        payload.fault.status = *status;
        if (const auto first_seen = field(fields, "first-seen"); first_seen.has_value()) {
            const auto parsed = parse_instant(*first_seen);
            if (!parsed.has_value()) {
                problem = "first-seen is not a canonical UTC instant";
                return false;
            }
            payload.fault.first_seen = *parsed;
        }
    } else if (kind_text == "maintenance") {
        payload.kind = EvidenceKind::MaintenanceStatement;
        const std::string kind_value = required(fields, "kind", problem);
        const std::string state_value = required(fields, "state", problem);
        const std::string start_text = required(fields, "start", problem);
        const std::string end_text = required(fields, "end", problem);
        const std::string mask_text = field(fields, "mask").value_or("none");
        if (!problem.empty()) {
            return false;
        }
        const auto maintenance_kind = parse_maintenance_kind(kind_value);
        const auto maintenance_state = parse_maintenance_state(state_value);
        const auto start = parse_instant(start_text);
        const auto end = parse_instant(end_text);
        const auto mask = parse_mask_scope(mask_text);
        if (!maintenance_kind.has_value() || !maintenance_state.has_value() || !start.has_value() ||
            !end.has_value() || !mask.has_value()) {
            problem = "maintenance needs a canonical kind, state, start, end and mask";
            return false;
        }
        payload.maintenance.kind = *maintenance_kind;
        payload.maintenance.state = *maintenance_state;
        payload.maintenance.window_start = *start;
        payload.maintenance.window_end = *end;
        payload.maintenance.mask = *mask;
    } else if (kind_text == "firmware") {
        payload.kind = EvidenceKind::FirmwareStatement;
        const std::string version_text = required(fields, "version", problem);
        const std::string compliance_text = required(fields, "compliance", problem);
        if (!problem.empty()) {
            return false;
        }
        const auto version = VersionText::parse(version_text);
        const auto compliance = parse_firmware_compliance(compliance_text);
        if (!version.has_value()) {
            problem = "version '" + version_text + "' is not a canonical version label";
            return false;
        }
        if (!compliance.has_value()) {
            problem = "compliance '" + compliance_text + "' is not a canonical firmware compliance";
            return false;
        }
        if (const auto component_text = field(fields, "component"); component_text.has_value()) {
            const auto component = ComponentId::parse(*component_text);
            if (!component.has_value()) {
                problem = "component is not a canonical token";
                return false;
            }
            payload.firmware.component = *component;
        }
        payload.firmware.version = *version;
        payload.firmware.compliance = *compliance;
        if (const auto baseline_text = field(fields, "baseline"); baseline_text.has_value()) {
            const auto baseline = VersionText::parse(*baseline_text);
            if (!baseline.has_value()) {
                problem = "baseline is not a canonical version label";
                return false;
            }
            payload.firmware.baseline = *baseline;
        }
    } else {
        problem = "statement kind '" + kind_text + "' is not one of identity, lifecycle, telemetry, fault, "
                  "maintenance, firmware";
        return false;
    }

    const auto subject = EvidenceSubject::make(*asset, AssetGeneration(static_cast<std::uint32_t>(generation)));
    if (!subject.has_value()) {
        problem = "the asset and generation do not form a valid subject";
        return false;
    }
    if (const auto id_text = field(fields, "evidence"); id_text.has_value()) {
        const auto id = EvidenceId::parse(*id_text);
        if (!id.has_value()) {
            problem = "evidence is not a canonical identifier";
            return false;
        }
        parts.id = *id;
    } else {
        const auto id = EvidenceId::generate();
        if (!id.has_value()) {
            problem = "the platform entropy source is unavailable, so no identifier could be generated";
            return false;
        }
        parts.id = id.value();
    }
    parts.subject = *subject;
    parts.provenance = *built_provenance;
    parts.payload = payload;
    return true;
}

[[nodiscard]] ObservatoryOptions options_for(const Arguments& arguments) {
    ObservatoryOptions options;
    options.store_root = arguments.store;
    options.create_if_missing = arguments.create;
    return options;
}

[[nodiscard]] int report_error(const Error& error) {
    std::cerr << "asset-health: " << error.to_string() << "\n";
    if (is_storage_error(error.code()) || error.code() == ErrorCode::InteriorCorruption ||
        error.code() == ErrorCode::StoreCorrupt) {
        return kExitIntegrity;
    }
    return kExitFailed;
}

int command_ingest(const Arguments& arguments) {
    std::ifstream file;
    std::istream* input = &std::cin;
    if (!arguments.file.empty()) {
        file.open(arguments.file);
        if (!file) {
            std::cerr << "asset-health: cannot open " << arguments.file << "\n";
            return kExitUsage;
        }
        input = &file;
    }
    ObservatoryOptions options = options_for(arguments);
    options.ingest_workers = 0;
    auto opened = Observatory::open(options);
    if (!opened) {
        return report_error(opened.error());
    }
    Observatory observatory = std::move(opened.value());

    int status = kExitOk;
    std::size_t line_number = 0;
    std::string line;
    while (std::getline(*input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        EvidenceRecord::Parts parts;
        std::string problem;
        if (!build_record(line, parts, problem)) {
            std::cerr << "asset-health: line " << line_number << ": " << problem << "\n";
            status = kExitUsage;
            break;
        }
        auto outcome = observatory.ingest(parts);
        if (!outcome) {
            std::cerr << "asset-health: line " << line_number << ": " << outcome.error().to_string() << "\n";
            status = report_error(outcome.error());
            break;
        }
        if (arguments.json) {
            std::cout << to_json(outcome.value());
        } else if (outcome.value().admitted) {
            std::cout << "line " << line_number << ": admitted " << outcome.value().evidence_id.to_string()
                      << " at generation " << outcome.value().generation.to_string()
                      << (outcome.value().duplicate_delivery ? " (already admitted)" : "") << "\n";
        } else {
            std::cout << "line " << line_number << ": refused " << error_code_name(outcome.value().refusal_code)
                      << ": " << outcome.value().detail << "\n";
        }
    }
    return status;
}

[[nodiscard]] Outcome<AssetRefId> argument_asset(const Arguments& arguments) {
    if (arguments.asset.empty()) {
        return Error(ErrorCode::InvalidInput, "--asset is required");
    }
    const auto asset = AssetRefId::parse(arguments.asset);
    if (!asset.has_value()) {
        return Error(ErrorCode::MalformedAssetReference,
                     "--asset is not a canonical lowercase hyphenated identifier");
    }
    return *asset;
}

[[nodiscard]] Outcome<Instant> argument_instant(const Arguments& arguments) {
    if (arguments.at.empty()) {
        return SystemClock{}.now();
    }
    const auto instant = parse_instant(arguments.at);
    if (!instant.has_value()) {
        return Error(ErrorCode::MalformedTimestamp, "--at is not a canonical UTC instant");
    }
    return *instant;
}

int command_assess(const Arguments& arguments) {
    const auto asset = argument_asset(arguments);
    if (!asset) {
        return report_error(asset.error());
    }
    const auto at = argument_instant(arguments);
    if (!at) {
        return report_error(at.error());
    }
    auto opened = Observatory::open(options_for(arguments));
    if (!opened) {
        return report_error(opened.error());
    }
    Observatory observatory = std::move(opened.value());
    auto assessment = arguments.publish ? observatory.assess_and_publish(asset.value(), at.value())
                                        : observatory.assess(asset.value(), at.value());
    if (!assessment) {
        return report_error(assessment.error());
    }
    if (arguments.json) {
        std::cout << to_json(assessment.value());
        return kExitOk;
    }
    std::cout << assessment.value().explain();
    return kExitOk;
}

int command_history(const Arguments& arguments) {
    const auto asset = argument_asset(arguments);
    if (!asset) {
        return report_error(asset.error());
    }
    auto opened = Observatory::open(options_for(arguments));
    if (!opened) {
        return report_error(opened.error());
    }
    Observatory observatory = std::move(opened.value());
    auto history = observatory.history(asset.value());
    if (!history) {
        return report_error(history.error());
    }
    if (arguments.json) {
        std::cout << to_json(history.value());
        return kExitOk;
    }
    std::cout << history.value().to_string();
    return kExitOk;
}

int command_evidence(const Arguments& arguments) {
    const auto asset = argument_asset(arguments);
    if (!asset) {
        return report_error(asset.error());
    }
    auto opened = Observatory::open(options_for(arguments));
    if (!opened) {
        return report_error(opened.error());
    }
    Observatory observatory = std::move(opened.value());
    auto records = observatory.evidence_for(asset.value());
    if (!records) {
        return report_error(records.error());
    }
    if (arguments.json) {
        std::cout << to_json(records.value());
        return kExitOk;
    }
    for (const EvidenceRecord& record : records.value()) {
        std::cout << record.to_string() << "\n";
    }
    return kExitOk;
}

int command_refusals(const Arguments& arguments) {
    const auto asset = argument_asset(arguments);
    if (!asset) {
        return report_error(asset.error());
    }
    auto opened = Observatory::open(options_for(arguments));
    if (!opened) {
        return report_error(opened.error());
    }
    Observatory observatory = std::move(opened.value());
    auto refusals = observatory.refusals_for(asset.value());
    if (!refusals) {
        return report_error(refusals.error());
    }
    if (arguments.json) {
        std::cout << "[";
        for (std::size_t index = 0; index < refusals.value().size(); ++index) {
            if (index != 0) {
                std::cout << ",";
            }
            std::cout << to_json(refusals.value()[index]);
        }
        std::cout << "]\n";
        return kExitOk;
    }
    for (const RefusalRecord& refusal : refusals.value()) {
        std::cout << error_code_name(refusal.code) << ": " << refusal.detail << " (source "
                  << refusal.source.str() << ", received " << refusal.received_at.to_millisecond_string() << ")\n";
    }
    return kExitOk;
}

int command_stats(const Arguments& arguments) {
    auto opened = Observatory::open(options_for(arguments));
    if (!opened) {
        return report_error(opened.error());
    }
    Observatory observatory = std::move(opened.value());
    StoreOptions options;
    options.root = arguments.store;
    options.read_only = true;
    auto store = Store::open(options);
    if (!store) {
        return report_error(store.error());
    }
    const StoreStatePtr state = store.value().state();
    if (state == nullptr) {
        std::cerr << "asset-health: the store has no published state\n";
        return kExitFailed;
    }
    if (arguments.json) {
        std::cout << statistics_json(*state);
        return kExitOk;
    }
    std::cout << "epoch " << state->epoch.to_string() << " generation " << state->generation.to_string()
              << " evidence " << state->evidence.size() << " history " << state->history.size() << " refusals "
              << state->refusals.size() << " streams " << state->source_streams.size() << "\n";
    return kExitOk;
}

int command_sources(const Arguments& arguments) {
    StoreOptions options;
    options.root = arguments.store;
    options.read_only = true;
    auto store = Store::open(options);
    if (!store) {
        return report_error(store.error());
    }
    const StoreStatePtr state = store.value().state();
    if (state == nullptr) {
        std::cerr << "asset-health: the store has no published state\n";
        return kExitFailed;
    }
    if (arguments.json) {
        std::cout << sources_json(*state);
        return kExitOk;
    }
    for (const SourceStreamState& stream : state->source_streams) {
        std::cout << stream.source.str() << " kind " << to_string(stream.kind) << " epoch "
                  << stream.epoch.to_string() << " last-sequence " << stream.last_sequence.to_string()
                  << " last-observed " << stream.last_observed_at.to_millisecond_string() << "\n";
    }
    return kExitOk;
}

int command_verify(const Arguments& arguments) {
    const auto audit = Store::audit_at(arguments.store);
    if (!audit) {
        return report_error(audit.error());
    }
    if (arguments.json) {
        std::cout << to_json(audit.value());
    } else {
        std::cout << audit.value().to_string() << "\n";
    }
    return audit.value().ok ? kExitOk : kExitIntegrity;
}

int command_policy(const Arguments& arguments) {
    if (arguments.json) {
        std::cout << to_json(HealthPolicy::standard());
        return kExitOk;
    }
    std::cout << HealthPolicy::standard().to_string();
    std::cout << "fingerprint " << HealthPolicy::standard().fingerprint() << "\n";
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    std::string problem;
    if (!parse_arguments(argc, argv, arguments, problem)) {
        std::cerr << "asset-health: " << problem << "\n";
        print_usage(std::cerr);
        return kExitUsage;
    }
    if (arguments.command == "help" || arguments.command == "--help" || arguments.command == "-h") {
        print_usage(std::cout);
        return kExitOk;
    }
    if (arguments.command == "--version") {
        std::cout << "asset-health " << kVersion << "\n";
        return kExitOk;
    }
    if (arguments.command == "ingest") {
        return command_ingest(arguments);
    }
    if (arguments.command == "assess") {
        return command_assess(arguments);
    }
    if (arguments.command == "history") {
        return command_history(arguments);
    }
    if (arguments.command == "evidence") {
        return command_evidence(arguments);
    }
    if (arguments.command == "refusals") {
        return command_refusals(arguments);
    }
    if (arguments.command == "stats") {
        return command_stats(arguments);
    }
    if (arguments.command == "sources") {
        return command_sources(arguments);
    }
    if (arguments.command == "verify") {
        return command_verify(arguments);
    }
    if (arguments.command == "policy") {
        return command_policy(arguments);
    }
    std::cerr << "asset-health: unknown command '" << arguments.command << "'\n";
    print_usage(std::cerr);
    return kExitUsage;
}
