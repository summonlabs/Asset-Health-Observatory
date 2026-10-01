// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "test_harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace asset_test {
namespace {

std::vector<TestCase>& cases() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Current {
    std::string suite;
    std::string name;
    std::uint64_t failures = 0;
};

Current& current() {
    static Current value;
    return value;
}

std::uint64_t& failure_total() {
    static std::uint64_t total = 0;
    return total;
}

}  // namespace

void register_case(std::string suite, std::string name, std::function<void()> body) {
    TestCase test;
    test.suite = std::move(suite);
    test.name = std::move(name);
    test.body = std::move(body);
    cases().push_back(std::move(test));
}

void record_failure(const char* file, int line, std::string message) {
    ++current().failures;
    ++failure_total();
    std::cout << "FAIL " << current().suite << "." << current().name << " " << file << ":" << line << ": " << message
              << "\n";
}

void log_line(std::string_view text) { std::cout << "  " << text << "\n"; }

std::uint64_t failure_count() noexcept { return failure_total(); }

int run_all(int argc, char** argv) {
    std::string filter;
    bool verbose = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind("--filter=", 0) == 0) {
            filter = argument.substr(9);
        } else if (argument == "--verbose") {
            // Prints each case as it starts. A case that crashes or hangs without
            // returning leaves its own name as the last line, which is how a
            // defect is located rather than guessed at.
            verbose = true;
        }
    }
    std::size_t executed = 0;
    std::size_t failed_cases = 0;
    for (const TestCase& test : cases()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos &&
            test.suite.find(filter) == std::string::npos) {
            continue;
        }
        current().suite = test.suite;
        current().name = test.name;
        current().failures = 0;
        if (verbose) {
            std::cout << "case " << test.suite << "." << test.name << "\n" << std::flush;
        }
        test.body();
        ++executed;
        if (current().failures != 0) {
            ++failed_cases;
        }
    }
    std::cout << "cases: " << executed << ", failed: " << failed_cases << ", failures: " << failure_total() << "\n";
    return failed_cases == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace asset_test

int main(int argc, char** argv) { return ::asset_test::run_all(argc, argv); }
