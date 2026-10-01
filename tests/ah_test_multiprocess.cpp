// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Real multiprocess behaviour: a second operating-system process holds the write
// lock, and two operating-system processes race for it. The programs are started
// directly, without a command interpreter, so nothing here depends on how a shell
// quotes a path.
//
// The waits are bounded polls for a file another process creates. Exhausting a
// bound fails the case; it never turns an unfinished case into a pass, and no
// timeout is applied to the suite as a whole.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "asset_health/persistence.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

constexpr const char* kHelper = AH_HELPER_PATH;

}  // namespace

AH_TEST(multiprocess, a_second_process_is_locked_out_of_a_store_held_by_another) {
    const TempDirectory directory("multiprocess-lock");
    const std::filesystem::path store = directory.path() / "store";
    const std::filesystem::path marker = directory.path() / "marker.txt";
    const std::filesystem::path release = directory.path() / "release.txt";

    StoreOptions options;
    options.root = store;
    auto prepared = Store::open(options);
    AH_REQUIRE(prepared.has_value());
    prepared.value().close();

    const ProcessHandle holder = start_process(kHelper, {"hold", store.string(), marker.string(), release.string()});
    AH_REQUIRE_MSG(holder.valid(), "the helper process could not be started");

    AH_REQUIRE_MSG(wait_for_line(marker, "holding"), "the helper process never announced that it held the lock");

    // A second writable open in this process must be refused, and the refusal
    // must name the lock rather than something else.
    auto blocked = Store::open(options);
    AH_REQUIRE_MSG(!blocked.has_value(), "a second writer opened a store another process holds");
    AH_CHECK(blocked.error().code() == ErrorCode::StoreLocked);

    // A read-only open meanwhile is allowed and sees a committed generation.
    StoreOptions read_only = options;
    read_only.read_only = true;
    auto reader = Store::open(read_only);
    AH_REQUIRE_MSG(reader.has_value(), reader.error().to_string());
    reader.value().close();

    write_text_file(release, "go\n");
    const int holder_status = wait_process(holder);
    AH_CHECK_MSG(holder_status == 0, "the helper process exited with " + std::to_string(holder_status));

    auto after = Store::open(options);
    AH_REQUIRE_MSG(after.has_value(), after.error().to_string());
    after.value().close();
}

AH_TEST(multiprocess, two_racing_writers_never_lose_a_committed_record) {
    const TempDirectory directory("multiprocess-race");
    const std::filesystem::path store = directory.path() / "store";
    StoreOptions options;
    options.root = store;
    auto prepared = Store::open(options);
    AH_REQUIRE(prepared.has_value());
    prepared.value().close();

    const std::filesystem::path first_result = directory.path() / "first.txt";
    const std::filesystem::path second_result = directory.path() / "second.txt";
    const ProcessHandle first =
        start_process(kHelper, {"race", store.string(), "3", first_result.string()});
    const ProcessHandle second =
        start_process(kHelper, {"race", store.string(), "3", second_result.string()});
    AH_REQUIRE(first.valid());
    AH_REQUIRE(second.valid());
    const int first_status = wait_process(first);
    const int second_status = wait_process(second);
    AH_CHECK_MSG(first_status == 0, "the first racer exited with " + std::to_string(first_status));
    AH_CHECK_MSG(second_status == 0, "the second racer exited with " + std::to_string(second_status));

    const std::string first_text = read_text_file(first_result);
    const std::string second_text = read_text_file(second_result);
    std::size_t successes = 0;
    for (const std::string& text : {first_text, second_text}) {
        if (text.find("wrote") != std::string::npos) {
            ++successes;
            continue;
        }
        // A racer that did not write must have been refused the lock, not failed
        // for some other reason.
        AH_CHECK_MSG(text.find("store_locked") != std::string::npos, text);
    }
    AH_CHECK(successes >= 1);

    // Whatever the interleaving, the store holds exactly the records the winning
    // process committed, and its integrity is intact.
    auto opened = Store::open(options);
    AH_REQUIRE_MSG(opened.has_value(), opened.error().to_string());
    AH_CHECK(opened.value().state()->evidence.size() == successes * 3);
    const auto audit = opened.value().audit();
    AH_REQUIRE(audit.has_value());
    AH_CHECK_MSG(audit->ok, audit->to_string());
    opened.value().close();
}
