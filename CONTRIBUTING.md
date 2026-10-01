# Contributing

Asset Health Observatory answers a question about physical equipment: given the
evidence, what health state is supported and why. A defect here is a wrong answer
about a machine, so contributions are judged first on whether they preserve the
invariants below, and only then on style.

## Build and test

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release --parallel
    ctest --test-dir build -C Release --output-on-failure

Useful variants:

    # Debug, with the standard library's assertions and iterator debugging enabled
    cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-debug --config Debug --parallel
    ctest --test-dir build-debug -C Debug --output-on-failure

    # AddressSanitizer over the whole suite
    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DASSET_HEALTH_SANITIZE=address
    cmake --build build-asan --config RelWithDebInfo --parallel
    ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure

    # Benchmarks (completed operations only; the small set runs in seconds)
    cmake --build build --config Release --target run_benchmarks_small

On MSVC the sanitizer build needs the compiler's runtime library on the loader
path. Copy clang_rt.asan_dynamic-x86_64.dll from the toolchain's bin/Hostx64/x64
directory next to each test executable, or run the suite from a developer prompt
that has that directory on PATH.

Warnings are errors by default (ASSET_HEALTH_WARNINGS_AS_ERRORS=ON). A first-party
warning is a defect: fix it rather than suppressing it, and add a suppression only
with a comment that says which defect it cannot describe.

## Invariants a change must not weaken

1. **One canonical spelling per value.** Every typed value validates on
   construction and has exactly one textual form. Never add a fallback that
   accepts a second spelling, and never normalise hostile input into validity.
2. **The decoder is the trust boundary.** Durable payloads are parsed by code that
   never reads past its buffer, never allocates from a declared count before
   checking it against the configured bound and the remaining input, and reports a
   structured status instead of throwing.
3. **A commit is all or nothing, and the commit point is the atomic replacement of
   CURRENT.** Before it moves the commit does not exist; after it moves the commit
   is durable. A generation file that CURRENT does not name is never adopted as
   state.
4. **Recovered dynamic evidence is not fresh evidence.** A session that died
   leaves nothing confirmed since it stopped, so its telemetry and fault
   statements stop counting until the feed speaks again. An orderly restart is a
   different case and is treated as one.
5. **Missing evidence is unknown, never healthy.** A required metric with no
   usable reading caps the answer at Unknown.
6. **Conflict is preserved, not resolved away.** Two sources of the same class
   that disagree produce a finding that names both readings; neither is dropped
   and neither is preferred. One source reporting twice is a series of readings,
   not a disagreement.
7. **Observation is not authority.** Evidence is admitted only into the domain its
   source speaks for. Seeing another authority's statement never makes this
   runtime that authority.
8. **No lock is held while user code runs, and no lock is taken twice.** The lock
   order is the ingest mutex, then the store's commit mutex, then leaf mutexes;
   the queue mutex is never held across a commit, and workers are joined without
   holding anything they need.
9. **An assessment is a function of its inputs.** No clock is read inside the
   evaluator, every enumeration has one total order, and no arithmetic is
   approximate: risk totals are exact rationals and quantities are fixed point.
10. **No telemetry.** Nothing is reported anywhere except where the caller asked
    for it.

## Tests

The suite is the evidence for the guarantees, so a change to behaviour is a change
to tests:

* Add a case that fails before the change and passes after it. A defect fix
  without a case that reproduces the defect is incomplete.
* Assert the refusal: the error code, and the detail a caller can act on. A test
  that only checks that a call failed does not say why the answer is no.
* Randomized cases use the seeded generator in tests/test_support.hpp and print
  their seed, so a failure is reproducible from it.
* Processes are started directly rather than through a command interpreter, so a
  build path containing a space is not part of what is under test.
* The harness has no timeout and no watchdog, deliberately: a hang is a defect to
  be diagnosed, not a case to be killed. Do not add one. Where a case waits for
  another process, the wait is a bounded poll that fails the case when the bound
  is exhausted.

## Style

* C++20, no extensions, no third-party dependencies.
* Every file starts with the copyright line and the SPDX identifier
  (SPDX-License-Identifier: Apache-2.0).
* Comments explain why a thing is the way it is, at the point where the reasoning
  is non-obvious. Comments that restate the code are noise; comments that record a
  decision are the point.
* Public headers document the contract, including what a caller must not do and
  which error a refusal produces.
* Keep the diff focused: one defect, one change, one test.

## Submitting

1. Build and run the full suite in Release and Debug, and under AddressSanitizer
   if the change touches parsing, persistence, evaluation or concurrency.
2. Describe what the change makes true that was not true before, and which case
   proves it.
3. Do not include generated artefacts, build trees, benchmark output, or editor
   state.
4. Contributions are accepted under the Apache License 2.0, as stated in LICENSE,
   with no additional terms and no contributor licence agreement to sign.

## Reporting a defect

A useful report contains the operation, the exact evidence or store layout, the
observed result, and what the documented behaviour is. If the defect is in durable
state, keep the store directory: the generation files, CURRENT and meta are the
evidence, and the verify subcommand reproduces the integrity audit:

    asset-health verify --store <directory>
