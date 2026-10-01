// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Minimal test harness.
//
// Deliberately small: a registry of cases, an assertion set that records failures
// with file and line, and a runner that reports every failure and exits non-zero
// when any case failed. There is no timeout mechanism of any kind: a case that
// hangs is a defect to be diagnosed, not a case to be killed, so the suite runs
// every case to completion and the process exits only when every case has
// returned.

#ifndef ASSET_HEALTH_TEST_HARNESS_HPP
#define ASSET_HEALTH_TEST_HARNESS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace asset_test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

/// Registers a case. Called by the AH_TEST macro at namespace scope.
void register_case(std::string suite, std::string name, std::function<void()> body);

/// Records a failure for the currently running case.
void record_failure(const char* file, int line, std::string message);

/// Prints a diagnostic line for the currently running case.
void log_line(std::string_view text);

/// Marks a bound assertion result as used, so a strict-warning build does not
/// report an unused variable for a binding whose only purpose was to assert.
template <typename T>
const T& bound_value(const T& value) noexcept {
    return value;
}

/// Runs every registered case. Returns the number of failed cases.
int run_all(int argc, char** argv);

/// Number of cases that failed so far in the current process.
[[nodiscard]] std::uint64_t failure_count() noexcept;

}  // namespace asset_test

#define AH_TEST(suite_name, case_name)                                                              \
    static void suite_name##_##case_name##_body();                                                  \
    namespace {                                                                                     \
    const bool suite_name##_##case_name##_registered = [] {                                          \
        ::asset_test::register_case(#suite_name, #case_name, &suite_name##_##case_name##_body);      \
        return true;                                                                                \
    }();                                                                                            \
    }                                                                                               \
    static void suite_name##_##case_name##_body()

#define AH_CHECK(condition)                                                                \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            ::asset_test::record_failure(__FILE__, __LINE__, "check failed: " #condition);  \
        }                                                                                  \
    } while (false)

#define AH_CHECK_MSG(condition, message)                                                              \
    do {                                                                                              \
        if (!(condition)) {                                                                           \
            ::asset_test::record_failure(__FILE__, __LINE__,                                          \
                                         std::string("check failed: " #condition " -- ") +            \
                                             std::string(message));                                   \
        }                                                                                             \
    } while (false)

/// Checks the condition and abandons the case when it does not hold, so a case
/// that cannot continue reports one failure rather than one per dependent
/// assertion.
#define AH_REQUIRE(condition)                                                                        \
    do {                                                                                             \
        if (!(condition)) {                                                                          \
            ::asset_test::record_failure(__FILE__, __LINE__, "required check failed: " #condition);   \
            return;                                                                                  \
        }                                                                                            \
    } while (false)

#define AH_REQUIRE_MSG(condition, message)                                                           \
    do {                                                                                             \
        if (!(condition)) {                                                                          \
            ::asset_test::record_failure(__FILE__, __LINE__,                                         \
                                         std::string("required check failed: " #condition " -- ") +   \
                                             std::string(message));                                  \
            return;                                                                                  \
        }                                                                                            \
    } while (false)

/// Requires an Outcome to hold a value and binds it to the given name.
#define AH_REQUIRE_OK(name, outcome)                                                                 \
    auto name##_outcome = (outcome);                                                                 \
    if (!name##_outcome.has_value()) {                                                               \
        ::asset_test::record_failure(__FILE__, __LINE__,                                             \
                                     std::string("expected success but got: ") +                     \
                                         name##_outcome.error().to_string());                        \
        return;                                                                                      \
    }                                                                                                \
    auto& name = name##_outcome.value();                                                             \
    (void)::asset_test::bound_value(name)

/// Requires an optional to hold a value and binds it to the given name.
#define AH_REQUIRE_PRESENT(name, optional_value)                                                     \
    auto name##_optional = (optional_value);                                                         \
    if (!name##_optional.has_value()) {                                                              \
        ::asset_test::record_failure(__FILE__, __LINE__,                                             \
                                     "expected a value: " #optional_value);                          \
        return;                                                                                      \
    }                                                                                                \
    const auto& name = *name##_optional

/// Requires an Outcome to fail with the given code and binds the error to the
/// given name.
#define AH_REQUIRE_ERROR(name, outcome, expected_code)                                               \
    auto name##_outcome = (outcome);                                                                 \
    if (name##_outcome.has_value()) {                                                                \
        ::asset_test::record_failure(__FILE__, __LINE__,                                             \
                                     "expected rejection " #expected_code " but the call succeeded");  \
        return;                                                                                      \
    }                                                                                                \
    const ::asset_health::Error& name = name##_outcome.error();                                      \
    if (name.code() != (expected_code)) {                                                            \
        ::asset_test::record_failure(                                                                \
            __FILE__, __LINE__,                                                                      \
            std::string("expected rejection " #expected_code " but got ") +                          \
                std::string(::asset_health::error_code_name(name.code())) + " -- " + name.message()); \
        return;                                                                                      \
    }                                                                                                \
    (void)::asset_test::bound_value(name)

#endif  // ASSET_HEALTH_TEST_HARNESS_HPP
