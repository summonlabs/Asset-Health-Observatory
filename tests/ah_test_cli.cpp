// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// End to end through the installed command line: a script of statements is
// ingested, assessed, verified and inspected, and the exit codes are asserted
// because they are part of the tool's contract.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_test;

constexpr const char* kCli = AH_CLI_PATH;
constexpr const char* kAsset = "11111111-1111-4111-8111-111111111111";

/// Runs the command line with its output captured to a file and returns the exit
/// status.
///
/// The command goes through a small batch file rather than being handed to the
/// interpreter directly, because the interpreter's quote handling for a command
/// line that both quotes a program path and redirects output is not something a
/// test should be asserting on. A batch file is also how a real operator would
/// run this on Windows, and its exit status is carried out unchanged.
int run_cli(const std::string& arguments, const std::filesystem::path& output) {
    std::filesystem::path program(kCli);
    program.make_preferred();
    std::filesystem::path capture = output;
    capture.make_preferred();
    std::filesystem::path script = output.parent_path() / "run-cli.cmd";
    script.make_preferred();
    {
        std::ofstream stream(script, std::ios::trunc);
        stream << "@echo off\r\n";
        stream << "\"" << program.string() << "\" " << arguments << " > \"" << capture.string() << "\" 2>&1\r\n";
        stream << "exit /b %ERRORLEVEL%\r\n";
    }
    // One quoted token, no redirection on this command line: the interpreter
    // hands the batch file path through unchanged.
    return std::system(("\"" + script.string() + "\"").c_str());
}

[[nodiscard]] std::string read_all(const std::filesystem::path& file) {
    std::ifstream stream(file);
    std::string text;
    std::string line;
    while (std::getline(stream, line)) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

void write_script(const std::filesystem::path& file, const std::string& text) {
    std::ofstream stream(file, std::ios::trunc);
    stream << text;
}

}  // namespace

AH_TEST(cli, help_and_version_succeed) {
    const TempDirectory directory("cli-help");
    const auto output = directory.path() / "out.txt";
    AH_CHECK(run_cli("help", output) == 0);
    const std::string text = read_all(output);
    AH_CHECK(text.find("usage: asset-health") != std::string::npos);
    AH_CHECK(text.find("assess") != std::string::npos);
    AH_CHECK(run_cli("--version", output) == 0);
    AH_CHECK(read_all(output).find("asset-health 1.0.0") != std::string::npos);
    AH_CHECK(run_cli("no-such-command", output) != 0);
}

AH_TEST(cli, policy_reports_the_fingerprint) {
    const TempDirectory directory("cli-policy");
    const auto output = directory.path() / "policy.json";
    AH_CHECK(run_cli("policy --json", output) == 0);
    const std::string text = read_all(output);
    AH_CHECK(text.find("\"id\": \"standard-dccp-health\"") != std::string::npos);
    AH_CHECK(text.find("\"fingerprint\"") != std::string::npos);
}

AH_TEST(cli, a_scripted_session_ingests_assesses_and_verifies) {
    const TempDirectory directory("cli-session");
    const auto store = directory.path() / "store";
    const auto script = directory.path() / "evidence.txt";
    const auto output = directory.path() / "out.txt";

    write_script(script,
                 "# identity, lifecycle, telemetry and one fault\n"
                 "identity asset=" + std::string(kAsset) +
                     " gen=1 revision=4 source=registry-a source-kind=asset-registry class=authoritative"
                     " epoch=1 seq=1 observed=2026-01-01T00:00:00Z\n"
                 "lifecycle asset=" + std::string(kAsset) +
                     " gen=1 state=active source=lifecycle-a source-kind=hardware-lifecycle class=authoritative"
                     " epoch=1 seq=1 observed=2026-01-01T00:00:00Z\n"
                 "telemetry asset=" + std::string(kAsset) +
                     " gen=1 metric=temperature value=42.5c quality=good source=feed-a source-kind=telemetry-feed"
                     " class=peer epoch=1 seq=1 observed=2026-01-01T00:00:10Z\n"
                 "fault asset=" + std::string(kAsset) +
                     " gen=1 component=psu-1 code=psu-ovt severity=major status=active source=faults-a"
                     " source-kind=fault-feed class=peer epoch=1 seq=1 observed=2026-01-01T00:00:20Z\n");

    const std::string store_argument = " --store \"" + store.string() + "\"";
    AH_CHECK(run_cli("ingest" + store_argument + " --file \"" + script.string() + "\"", output) == 0);
    const std::string ingest_output = read_all(output);
    AH_CHECK(ingest_output.find("admitted") != std::string::npos);
    AH_CHECK(ingest_output.find("refused") == std::string::npos);

    AH_CHECK(run_cli("assess" + store_argument + " --asset " + std::string(kAsset) +
                         " --at 2026-01-01T00:00:30Z",
                     output) == 0);
    const std::string assessment = read_all(output);
    AH_CHECK(assessment.find("state critical") != std::string::npos);
    AH_CHECK(assessment.find("fault-active") != std::string::npos);
    AH_CHECK(assessment.find("risk-input") != std::string::npos);
    AH_CHECK(assessment.find("dependency") != std::string::npos);

    // Publishing records the answer in durable history.
    AH_CHECK(run_cli("assess" + store_argument + " --asset " + std::string(kAsset) +
                         " --at 2026-01-01T00:00:30Z --publish",
                     output) == 0);
    AH_CHECK(run_cli("history" + store_argument + " --asset " + std::string(kAsset) + " --json", output) == 0);
    const std::string history = read_all(output);
    AH_CHECK(history.find("\"entries\"") != std::string::npos);
    AH_CHECK(history.find("\"critical\"") != std::string::npos);

    AH_CHECK(run_cli("stats" + store_argument + " --json", output) == 0);
    AH_CHECK(read_all(output).find("\"evidence\": 4") != std::string::npos);

    AH_CHECK(run_cli("sources" + store_argument + " --json", output) == 0);
    AH_CHECK(read_all(output).find("registry-a") != std::string::npos);

    AH_CHECK(run_cli("evidence" + store_argument + " --asset " + std::string(kAsset), output) == 0);
    AH_CHECK(read_all(output).find("telemetry-reading") != std::string::npos);

    AH_CHECK(run_cli("verify" + store_argument, output) == 0);
    AH_CHECK(read_all(output).find("ok true") != std::string::npos);
}

AH_TEST(cli, a_malformed_line_is_a_usage_error_and_a_refusal_is_recorded) {
    const TempDirectory directory("cli-errors");
    const auto store = directory.path() / "store";
    const auto output = directory.path() / "out.txt";
    const std::string store_argument = " --store \"" + store.string() + "\"";

    const auto bad_line = directory.path() / "bad.txt";
    write_script(bad_line, "telemetry asset=not-an-identifier gen=1\n");
    AH_CHECK(run_cli("ingest" + store_argument + " --file \"" + bad_line.string() + "\"", output) == 1);
    AH_CHECK(read_all(output).find("asset") != std::string::npos);

    // A well-formed line that the observatory refuses is a different answer from
    // a line the tool cannot parse, and it is recorded rather than dropped.
    const auto refused_line = directory.path() / "refused.txt";
    write_script(refused_line,
                 "lifecycle asset=" + std::string(kAsset) +
                     " gen=1 state=active source=feed-a source-kind=telemetry-feed class=peer epoch=1 seq=1"
                     " observed=2026-01-01T00:00:00Z\n");
    AH_CHECK(run_cli("ingest" + store_argument + " --file \"" + refused_line.string() + "\"", output) == 0);
    AH_CHECK(read_all(output).find("refused authority_domain_violation") != std::string::npos);
    AH_CHECK(run_cli("refusals" + store_argument + " --asset " + std::string(kAsset), output) == 0);
    AH_CHECK(read_all(output).find("authority_domain_violation") != std::string::npos);
}

AH_TEST(cli, verify_reports_a_damaged_store_with_a_failing_status) {
    const TempDirectory directory("cli-damaged");
    const auto store = directory.path() / "store";
    const auto script = directory.path() / "evidence.txt";
    const auto output = directory.path() / "out.txt";
    const std::string store_argument = " --store \"" + store.string() + "\"";
    write_script(script, "telemetry asset=" + std::string(kAsset) +
                             " gen=1 metric=temperature value=42.5c quality=good source=feed-a"
                             " source-kind=telemetry-feed class=peer epoch=1 seq=1 observed=2026-01-01T00:00:00Z\n");
    AH_CHECK(run_cli("ingest" + store_argument + " --file \"" + script.string() + "\"", output) == 0);
    AH_CHECK(run_cli("verify" + store_argument, output) == 0);

    // Corrupt the newest generation: the audit must say so and exit non-zero.
    const auto generations = store / "generations";
    std::filesystem::path newest;
    for (const auto& entry : std::filesystem::directory_iterator(generations)) {
        if (newest.empty() || entry.path().filename().string() > newest.filename().string()) {
            newest = entry.path();
        }
    }
    AH_REQUIRE(!newest.empty());
    std::fstream stream(newest, std::ios::in | std::ios::out | std::ios::binary);
    stream.seekp(200);
    const char value = 'Z';
    stream.write(&value, 1);
    stream.close();

    AH_CHECK(run_cli("verify" + store_argument + " --json", output) == 3);
    const std::string audit = read_all(output);
    AH_CHECK(audit.find("\"ok\": false") != std::string::npos);
    AH_CHECK(audit.find("problem") != std::string::npos || audit.find("\"problems\"") != std::string::npos);
}
