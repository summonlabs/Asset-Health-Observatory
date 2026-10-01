// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Real restarts: a separate operating-system process writes, dies, or dies
// mid-write, and this process then reopens the store and says what it found.
// Nothing here is simulated in process.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "asset_health/observatory.hpp"
#include "store_format.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

constexpr const char* kHelper = AH_HELPER_PATH;
constexpr const char* kAssetText = "33333333-3333-4333-8333-333333333333";

/// Runs the helper and returns its exit status. The command is quoted because
/// the build directory contains spaces.
/// Runs the helper to completion and returns its exit status. The program is
/// started directly, so a build path containing a space needs no quoting.
[[nodiscard]] int run_helper(const std::vector<std::string>& arguments) {
    const ProcessHandle handle = start_process(kHelper, arguments);
    if (!handle.valid()) {
        return -1;
    }
    return wait_process(handle);
}

[[nodiscard]] std::filesystem::path newest_generation(const std::filesystem::path& store) {
    std::error_code error;
    GenerationSequence highest{};
    std::filesystem::path newest;
    for (const auto& entry : std::filesystem::directory_iterator(store / "generations", error)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto parsed = detail::parse_generation_file_name(entry.path().filename().string());
        if (parsed.has_value() && *parsed > highest) {
            highest = *parsed;
            newest = entry.path();
        }
    }
    return newest;
}

}  // namespace

AH_TEST(restart, a_clean_close_is_distinguishable_from_a_crash) {
    const TempDirectory directory("restart-clean");

    const std::filesystem::path clean_store = directory.path() / "clean";
    AH_REQUIRE(run_helper({"write", clean_store.string(), "5"}) == 0);
    StoreOptions options;
    options.root = clean_store;
    auto clean = Store::open(options);
    AH_REQUIRE_MSG(clean.has_value(), clean.error().to_string());
    AH_CHECK(clean.value().recovery().previous_close_clean);
    AH_CHECK(!clean.value().recovery().recovered());
    AH_CHECK(clean.value().recovery().evidence_records == 5);
    // A clean close demotes nothing: the readings are judged by their age.
    AH_CHECK(clean.value().recovery().dynamic_records_demoted == 0);
    clean.value().close();

    const std::filesystem::path crashed_store = directory.path() / "crashed";
    AH_REQUIRE(run_helper({"crash", crashed_store.string(), "5"}) == 0);
    StoreOptions crashed_options;
    crashed_options.root = crashed_store;
    auto crashed = Store::open(crashed_options);
    AH_REQUIRE_MSG(crashed.has_value(), crashed.error().to_string());
    AH_CHECK(!crashed.value().recovery().previous_close_clean);
    AH_CHECK(crashed.value().recovery().recovered());
    AH_CHECK(crashed.value().state()->recovered);
    AH_CHECK(crashed.value().recovery().evidence_records == 5);
    crashed.value().close();
}

AH_TEST(restart, a_process_that_died_mid_write_leaves_a_torn_tail_that_is_recovered) {
    const TempDirectory directory("restart-torn");
    const std::filesystem::path store = directory.path() / "store";
    AH_REQUIRE(run_helper({"torn", store.string(), "4"}) == 0);

    StoreOptions options;
    options.root = store;
    auto opened = Store::open(options);
    AH_REQUIRE_MSG(opened.has_value(), opened.error().to_string());
    AH_CHECK(opened.value().recovery().torn_tail_discarded);
    AH_CHECK(opened.value().recovery().recovered());
    AH_CHECK(!opened.value().recovery().previous_close_clean);
    // The generation that was torn is gone, and the state falls back to the
    // previous commit rather than to a partial one.
    AH_CHECK(opened.value().generation().value() < 6);
    AH_CHECK(opened.value().state()->recovered);
    opened.value().close();
}

AH_TEST(restart, an_observatory_reopened_after_a_crash_reports_recovered_evidence) {
    const TempDirectory directory("restart-observatory");
    const std::filesystem::path store = directory.path() / "store";
    AH_REQUIRE(run_helper({"crash", store.string(), "3"}) == 0);

    ObservatoryOptions options;
    options.store_root = store;
    auto opened = Observatory::open(options);
    AH_REQUIRE_MSG(opened.has_value(), opened.error().to_string());
    Observatory observatory = std::move(opened.value());
    AH_CHECK(observatory.recovery().recovered());
    const AssetRefId asset = AssetRefId::parse(kAssetText).value();
    auto records = observatory.evidence_for(asset);
    AH_REQUIRE(records.has_value());
    AH_CHECK(records.value().size() == 3);

    // The statements are all from the dead process's epoch, so they are not fresh
    // evidence and the assessment says so rather than reporting a healthy asset.
    auto assessment = observatory.assess(asset, *parse_instant("2026-06-01T00:00:10Z"));
    AH_REQUIRE(assessment.has_value());
    AH_CHECK(assessment.value().flags.contains(AssessmentFlag::StaleEvidence));
    AH_CHECK(assessment.value().state != HealthState::Healthy);
    observatory.close();

    // And the store is healthy afterwards: the recovery did not damage it.
    auto audit = Store::audit_at(store);
    AH_REQUIRE(audit.has_value());
    AH_CHECK_MSG(audit->ok, audit->to_string());
}

AH_TEST(restart, a_recovered_store_keeps_its_evidence_across_many_reopens) {
    const TempDirectory directory("restart-many");
    const std::filesystem::path store = directory.path() / "store";
    AH_REQUIRE(run_helper({"write", store.string(), "2"}) == 0);
    StoreEpoch previous{};
    for (int round = 0; round < 4; ++round) {
        StoreOptions options;
        options.root = store;
        auto opened = Store::open(options);
        AH_REQUIRE_MSG(opened.has_value(), opened.error().to_string());
        AH_CHECK(opened.value().state()->evidence.size() >= 2);
        if (round != 0) {
            AH_CHECK(opened.value().epoch() > previous);
        }
        previous = opened.value().epoch();
        opened.value().close();
    }
}
