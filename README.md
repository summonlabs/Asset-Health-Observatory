# Asset Health Observatory

Evidence-bound facility asset health observation.

Given the evidence an operator, an adjacent authority, or an instrument has
published about a physical asset, this runtime answers one question: **what health
state is supported, what is degraded or uncertain, which evidence is stale or
conflicting, what lifecycle and maintenance context matters, and what replacement
risk is explicitly justified** - and shows exactly which statements the answer
rests on.

It is a library and a command line tool. It is not a monitoring agent, not a
registry, and not a controller.

## Boundary and non-ownership

This repository owns observation, explanation, history, evidence correlation, and
explicit risk and degradation views. It owns nothing else.

It does **not** own, and refuses to act as:

| Concern | Owner |
| --- | --- |
| Asset identity, incarnation, identity revision | Asset Registry |
| Lifecycle state and lifecycle transitions | Hardware Lifecycle |
| Maintenance scheduling and work orders | Maintenance Coordinator |
| Firmware versions, baselines and compliance decisions | Firmware Baseline Manager |
| Incidents, quarantine, replacement, actuation | the authorities that own them |

Observation is not ownership. A lifecycle state that appears in this store is a
statement this runtime read; it is never a state this runtime set. Every mutating
operation in this repository operates on its own evidence set and its own store,
never on another runtime's state.

The boundary is enforced in code. Each source declares which authority domain it
speaks for, and a statement is admitted only into the domain that owns it
(AuthorityDomainViolation otherwise). A telemetry producer may not assert a
lifecycle state; the identity authority may not assert a temperature. The one
exception is deliberate and both directions are checked: modelled evidence must
declare itself modelled, and a measured feed may not claim to be modelled.

The canonical asset identity this runtime stores is the lowercase hyphenated
128-bit form the identity authority publishes, so the text crosses the boundary
unchanged. Parsing, validation and every conclusion drawn from it happen here, and
nothing is written back.

## Build

No third-party dependencies. C++20 and CMake 3.20 or newer.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release --parallel
    ctest --test-dir build -C Release --output-on-failure

The project builds Release and Debug with warnings as errors on the first-party
targets (/W4 /WX on MSVC, -Wall -Wextra -Wpedantic -Werror elsewhere) and produces
zero first-party warnings. AddressSanitizer is available through
-DASSET_HEALTH_SANITIZE=address.

Options: ASSET_HEALTH_BUILD_SHARED, ASSET_HEALTH_BUILD_CLI, ASSET_HEALTH_BUILD_TESTS,
ASSET_HEALTH_BUILD_EXAMPLES, ASSET_HEALTH_BUILD_BENCHMARKS,
ASSET_HEALTH_WARNINGS_AS_ERRORS, ASSET_HEALTH_SANITIZE.

## Quick start

Admit some evidence:

    asset-health ingest --store ./facility-store --file evidence.txt

where each line is one statement, the kind first:

    identity    asset=<id> gen=1 revision=4 source=registry-a \
                source-kind=asset-registry class=authoritative epoch=1 seq=1 observed=2026-01-01T00:00:00Z
    lifecycle   asset=<id> gen=1 state=active source=lifecycle-a \
                source-kind=hardware-lifecycle class=authoritative epoch=1 seq=1 observed=2026-01-01T00:00:00Z
    telemetry   asset=<id> gen=1 metric=temperature value=82.5c quality=good source=feed-a \
                source-kind=telemetry-feed class=peer epoch=1 seq=1 observed=2026-01-01T00:00:10Z
    fault       asset=<id> gen=1 component=pump-1 code=coolant-flow-low severity=major status=active \
                source=faults-a source-kind=fault-feed class=peer epoch=1 seq=1 observed=2026-01-01T00:00:20Z

Ask what it means:

    asset-health assess --store ./facility-store --asset <id> --at 2026-01-01T00:00:30Z

which prints the state, every finding with the statements it rests on, the
degradation indicators, the dependency list with each statement's freshness, and
the risk inputs:

    state critical flags unknown-firmware
    finding major fault-active supported impact=critical component=pump-1 evidence=<id> -- ...
    finding warning metric-critical-threshold supported impact=critical metric=temperature evidence=<id> -- ...
    dependency <id> fault-statement source faults-a class peer observed ... freshness fresh fresh=true
    risk-input active-fault-severity available=true raw-milli=4000 saturation-milli=4000 score=1/1 ...

Other commands: history, evidence, refusals, sources, stats, verify, policy, help.
The --json option switches any of them to canonical JSON. Exit codes: 0 success,
1 usage error, 2 refusal or failure, 3 store integrity failure.

## Library example

    #include "asset_health/observatory.hpp"
    using namespace asset_health;

    ObservatoryOptions options;
    options.store_root = "facility-store";
    auto opened = Observatory::open(options);
    if (!opened) { /* opened.error() says why, with a stable code */ }
    Observatory observatory = std::move(opened.value());

    EvidenceRecord::Parts parts;              // id, subject, provenance, payload
    parts.payload.kind = EvidenceKind::LifecycleStatement;
    parts.payload.lifecycle.state = LifecycleState::Active;
    auto admitted = observatory.ingest(parts);
    // A refusal is an answer, not an exception: admitted.value().admitted is false
    // and admitted.value().refusal_code says why.

    auto assessment = observatory.assess(asset, now);
    std::cout << assessment.value().explain();

The evaluator is also usable with no store at all, as a pure function:
evaluate(EvaluationRequest, HealthPolicy, EvaluationContext) in
asset_health/evaluation.hpp.

## Evidence model

**Kinds.** identity, lifecycle, maintenance, firmware, telemetry, fault. Each is
owned by exactly one authority domain and carries its own payload vocabulary.

**Provenance.** Every statement records its source, the source kind, the source
class (authoritative, peer, synthetic), the source's stream epoch and sequence, the
instant it was observed, the instant it was received, and an optional producer
version. The stream epoch is what makes a producer restart visible: a statement
from an epoch the source has already left behind belongs to a stream that no longer
exists.

**Freshness.** One function, no hidden inputs:

* a statement dated ahead of the evaluation instant by more than the policy's skew
  tolerance is future-dated, and a future-dated reading is not evidence;
* dynamic evidence (telemetry, faults) is fresh only while its source stream epoch
  is the newest seen, and only inside the same store epoch that committed it
  *unless that epoch was closed cleanly*. A session that died leaves nothing
  confirmed since it stopped, so its readings are reported as recovered and stop
  counting; an orderly restart leaves a reading to be judged by its age;
* static evidence (identity, lifecycle, maintenance, firmware) describes a state
  that persists until it is superseded, so it is judged by age alone.

**Precedence and conflict.** For one fact, an authoritative source outranks a peer,
which outranks a synthetic one; then the newer observation wins, then the higher
stream epoch, then the higher sequence, then the lexicographically smaller source.
Total, so the winner never depends on insertion order. Two sources of the same
class that disagree produce a conflict finding that names both readings - neither
is dropped and neither is preferred. One source reporting twice is a series of
readings, not a disagreement.

**Ordering.** Every record has one canonical order (asset, generation, observation
time, kind, fact key, source, epoch, sequence, identifier) and every listing,
dependency list and durable payload uses it. Evidence that arrives out of order is
placed by that order, never by arrival.

**Generation.** An observation about one incarnation is never an observation about
another. Statements naming an earlier or later generation of the same identity are
preserved, reported as superseded, and excluded from the assessment.

## Health model

States, in the order the answer aggregates: healthy, unknown, degraded, critical,
failed, and retired, which is terminal and orthogonal - a decommissioned asset has
no service health to judge.

Missing evidence is unknown, never healthy: a metric the policy marks required with
no usable reading caps the answer at unknown. Stale, recovered, future-dated and
superseded-epoch readings are reported with the reason they were not used rather
than being silently dropped. Uncertain, substituted and cached readings cannot
support a healthy verdict, but an excursion they report is still counted.

Context gates the answer rather than being folded into it:

* lifecycle: not-yet-in-service, service withheld, quarantined and draining each
  contribute their own finding and bound the answer at least at unknown
  (quarantined at least degraded); a decommissioned asset answers retired;
* maintenance: an active window of a kind the policy permits may set telemetry or
  fault findings aside. Masked findings are still reported, marked, and named with
  the window that masked them. Corrective and emergency work never masks by default,
  and a window that has run out stops masking;
* firmware: a missing, stale or unknown baseline is reported as unknown rather than
  assumed compliant, and feeds the risk view as an unknown input.

Every assessment carries a flag set that says what limits it - stale-evidence,
conflicting-evidence, missing-required-evidence, masked-by-maintenance,
unknown-firmware, unknown-lifecycle, identity-replaced, recovered-evidence,
synthetic-evidence, indeterminate, refused-evidence, no-evidence,
degradation-observed, evidence-listed-partially, source-loss-observed - so a
consumer reads the limit instead of inferring it.

**Degradation** is derived from a bounded window of fresh, good readings: net
movement in the metric's adverse direction, with a counter reset detected and
reported as a reset rather than a trend. The indicator names the samples.

## Replacement risk

Never an opaque score. The total is an exact rational:

    total = sum(weight_i * min(raw_i, saturation_i) / saturation_i) / sum(weight_i)

over the inputs that were available, and every input is printed with its raw value,
saturation point, normalised score, weight, contribution, explanation and the
statements it came from. Inputs: age in service, worst active fault severity, fault
recurrence, worsening degradation indicators, threshold excursions, corrective
maintenance burden, and firmware state. The completeness flag says whether every
input had evidence, and the band (low, moderate, high, severe, undetermined) is
justified against the configured boundaries in the output. Arithmetic is exact, so
the total is reproducible on any machine and comparable in a test.

## Policy

Every threshold, window, weight and mapping lives in one versioned HealthPolicy
value, which is fingerprinted and recorded with every published assessment, so a
change of threshold is visible in history rather than silently rewriting the past.
The standard policy (standard-dccp-health, version 1) sets, among others:
temperature required with warning at 75 C and critical at 85 C; power draw warning
at 1200 W; voltage two-sided 11-13 V warning and 10.5-13.5 V critical; fan speed
warning below 1500 rpm; any uncorrectable memory error a warning and ten a critical;
power-supply redundancy warning below two spares; battery health warning below 70
percent; wear warning at 80 percent. The dynamic freshness window is five minutes,
the static window thirty days, the clock skew tolerance five seconds, and the
degradation window 24 hours with at least three samples. The policy subcommand
prints the whole thing with its fingerprint.

A policy that contradicts itself is refused at construction: unordered thresholds,
a zero risk weight, backwards band boundaries, a non-positive window or too few
samples are all rejected, and a policy is only obtainable from HealthPolicy::make
or the built-in standard.

## Persistence

A store directory:

    <root>/meta                     format version, store identifier, creation time
    <root>/LOCK                     held for the whole lifetime of a writable open
    <root>/CLEAN                    present only between a clean close and the next open
    <root>/CURRENT                  the commit point: the generation a reader must load
    <root>/generations/gen-<n>.ahg  immutable generations, fixed-width names

**The exact commit point is the atomic replacement of CURRENT.** A commit writes a
complete generation to a temporary name, flushes it to the device, renames it into
place, and only then replaces CURRENT. Before that replacement the commit does not
exist; after it, the commit is durable. A generation file CURRENT does not name was
never committed and is never adopted as state - that rule is what keeps recovered
evidence from being promoted to current evidence.

Each generation carries a 128-byte header and a payload, each with its own CRC-32,
and the header names the store, the generation, the payload length and checksum, the
previous generation and its payload digest, and the retention boundary.
Enumerations inside the payload are stored as canonical names rather than ordinals,
so renumbering an enumeration in a later release cannot reinterpret an earlier
store, and a name outside the vocabulary is corruption rather than a value to guess
at. Every record is revalidated on load with the same predicate the store applies
when it admits one.

Recovery, on open:

* a torn tail - the newest committed generation shorter than its header declares -
  falls back to the previous committed generation, and the discard is reported;
* a complete generation that does not agree with itself (payload checksum, record
  invariant, header and payload disagreeing) is interior corruption and the store
  refuses to open with store_corrupt. A store that cannot be trusted is not
  half-opened;
* generations above the commit point were never committed: they are reported and
  removed so a later commit cannot collide with them;
* a missing commit point is refused (commit_point_missing), because without it there
  is no way to tell a committed generation from the debris of an interrupted one;
* an open publishes a strictly greater epoch, which fences out every writer holding
  a token from an earlier one, including across a restart.

Writes are guarded by a kernel-enforced single-writer lock (LockFileEx on Windows,
flock elsewhere), so a second writer is refused immediately rather than waiting. A
read-only open takes no lock and sees exactly one committed generation, because
generation files are immutable and CURRENT is replaced atomically.

The store records the identity of the last commits (sequence and idempotency key)
**inside the same generation as the mutation they describe**, so a retry that
arrives after a crash is answered from durable state instead of being applied twice;
a repeated sequence with a different key is refused as a conflict. The per-source
stream position (epoch, sequence, digest, and the identifier of the admitted
statement) is durable for the same reason: a replayed or stale delivery is caught
across a restart, not only within a session. Retention keeps a bounded number of
generations and reports the boundary, so an audit can tell a pruned generation from
a lost one.

## Concurrency

One ingest mutex serialises the read-decide-commit sequence of an admission, so a
stream check is atomic with the commit it authorises. The store's commit mutex is
taken inside it; queue and ticket mutexes are leaves and are never held across a
commit. An ingestion worker never holds the queue mutex while committing.

Readers never block writers. A published state is an immutable snapshot held by a
shared pointer; an assessment works on the snapshot it took and holds it for as long
as it likes. With ingest_workers configured, statements are queued and committed by
worker threads, and **statements from one source are committed in the order they
were queued** even with several workers, because a stream position is only
meaningful in order; different sources proceed in parallel. Shutdown drains the
queue, joins the workers without holding anything they need, and only then closes
the store. A caller can wait for an attempt to settle, and can ask for a queued
attempt to be cancelled: an attempt that has already been committed cannot be
cancelled and says so rather than claiming an effect that did not happen.

## Install and downstream use

    cmake --install build --config Release --prefix /some/prefix

installs the shared library, the public headers, the package configuration files,
the command line tool, and the documentation.

On Windows the shared library is installed to bin/, which is not on the default
loader search path. A consumer either places the library beside its executable or
runs with that directory on PATH; the validation below does the latter.

An out-of-tree consumer:

    cmake_minimum_required(VERSION 3.20)
    project(consumer LANGUAGES CXX)
    find_package(AssetHealthObservatory 1.0 REQUIRED)
    add_executable(consumer main.cpp)
    target_link_libraries(consumer PRIVATE AssetHealthObservatory::AssetHealthObservatory)

configured with -DCMAKE_PREFIX_PATH=/some/prefix. This is exercised as part of
validation: the library is installed to a clean prefix, a separate project outside
this repository is configured against it, built, run, and checked for the answer it
prints.

## Validation

Everything below was run on Windows 11 (10.0.26300) with MSVC 19.44.35207 and
CMake 4.3.2, x64. The test suite is 14 ctest cases covering unit, integration,
end-to-end, property, seeded randomized, adversarial, failure-injection, real
multiprocess and real restart behaviour. Every case passes in Release and Debug, and
the whole suite passes under AddressSanitizer.

What the cases establish, by suite:

* **values** - canonical spellings are the only accepted ones, exact rational
  arithmetic and its overflow refusals, time round trips including leap years and
  rejected impossible dates, fixed-point quantities, error codes round-tripping
  through their names, policy validation.
* **evidence** - the authority-domain boundary in both directions, mandatory
  synthetic labelling, per-kind payload validation, fact keys, precedence, canonical
  ordering, and a durable round trip of every kind of record including the header
  checksum, payload checksum and torn-tail distinctions.
* **freshness** - every branch of the freshness rule, including the boundary
  instant, recovery, source epoch supersession, future dating and unaged static
  state.
* **evaluation** - healthy, missing required evidence, stale and recovered readings,
  thresholds, conflicts preserved with both readings, active faults and stale faults,
  maintenance masking and masking refused, window expiry, lifecycle gating including
  end of life, identity replacement, firmware known and unknown, worsening trends,
  counter resets, risk recomputed from its printed inputs, and byte-identical
  explanations for identical inputs (including shuffled input order).
* **durability** - a commit that survives a reopen, epoch fencing, sequence
  advancement, idempotent replay and idempotency conflicts, torn-tail fallback,
  interior corruption refused, an uncommitted generation never adopted, a missing
  commit point refused, capacity bounds refusing rather than forgetting, read-only
  openness, lock exclusion, retention bounds, and an audit that reports a healthy
  store as healthy.
* **property** - seeded random evidence sets: the evaluation is a function of its
  inputs, shuffling the input does not change the answer, the codec round-trips
  random states, values round-trip through their canonical text, exact arithmetic is
  reversible where it is representable, an observatory holds exactly what it admitted
  in canonical order, and a reopened store reports the same evidence.
* **adversarial** - hostile generation files classified rather than trusted, a
  payload longer or shorter than declared, malformed statements refused with the
  specific reason and recorded as refusals, replays, dead-epoch deliveries,
  future-dated statements, duplicate identifiers with different content, capacity
  bounds, unknown assets, configuration bounds, and a store path that is a file.
* **concurrency** - 100 asynchronous statements across four streams admitted exactly
  once with four workers, concurrent readers observing only consistent canonical
  snapshots while 120 statements are committed, cancellation reporting what actually
  happened, closing with work queued settling every attempt, and repeated open and
  close without leaking a lock.
* **restart** - a real second process writes and closes, dies without closing, or
  dies after shortening the newest generation; this process then reopens and reports
  what it found, and distinguishes a clean close from a recovery.
* **multiprocess** - a real second process holds the write lock and this one is
  refused with store_locked; two real processes race for the lock and the store
  afterwards holds exactly what the winner committed, with an intact audit.
* **cli** - the installed command line ingests a script, assesses, publishes into
  history, reports statistics, sources, evidence and refusals, verifies a healthy
  store, reports a damaged one with a failing exit status, and distinguishes a usage
  error from a refusal.

## REAL, SYNTHETIC and UNSUPPORTED

**REAL.** The C++ implementation, the durable store, its recovery, the process lock,
the concurrency behaviour, the packaging, the installation and the out-of-tree
consumer build. The multiprocess and restart cases run real operating system
processes, and the recovery cases work on real files that were really truncated or
really corrupted.

**SYNTHETIC.** All evidence used by the test suite, the examples and the benchmarks
is generated by this repository. No facility instrument, sensor, building management
system, DCIM system, electrical or cooling device produced any input. The vocabulary
is modelled on a liquid-cooled accelerator facility, and the standard policy's
thresholds are a defensible judgement about one, but they are policy values, not
measurements.

**UNSUPPORTED.** Nothing here has been run against real facility hardware, a real
BMS or DCIM system, a real electrical or cooling plant, or a multi-node deployment.
No claim is made about behaviour under real sensor noise, real clock behaviour, real
network partitions between this runtime and its producers, or real fault storms.
Statements from a real producer would need their own validation before the
conclusions here could be trusted for them.

## Benchmarks

Measured on the machine above, Release x64, small set (run_benchmarks_small). Every
number is completed work: the operations counted are the ones that returned.

| Benchmark | Completed operations | Time | Per operation |
| --- | --- | --- | --- |
| ingest-commit | 200 | 0.991 s | 4955 us |
| assess-published-snapshot | 500 | 0.166 s | 333 us |
| evaluate-pure-function (256 statements) | 2000 | 0.620 s | 310 us |
| reopen-and-recover (200 records, generation 201) | 1 | 0.0040 s | 3961 us |

The cost model is stated next to the numbers because it is part of the result: a
commit rewrites the whole evidence set, so ingest cost grows with the number of
records the store holds (the run above holds up to 200). This is a deliberate trade:
it makes the commit point a single atomic rename and the recovered state exactly the
committed state, at the price of a linear write. The evaluator is linear in the
statements about one asset.

The label on each measurement says which part is synthetic: the input is always
generated by the benchmark itself, while the durable commit, the evaluation and the
recovery are real.

## Limitations

* One writer per store directory at a time; concurrent writers are refused rather
  than merged.
* A commit rewrites the whole evidence set, so ingest throughput is bounded by the
  size of the store rather than by the size of the change.
* The evidence set for one asset is bounded (4096 records by default) and the store
  refuses rather than evicting: an observatory that forgets evidence cannot explain
  its findings. A deployment that needs longer retention must compact or export.
* The metric vocabulary is closed. A producer with a property outside it must model
  it as the closest listed metric and say so; adding a metric is a vocabulary change,
  not a configuration change.
* Thresholds, weights and windows are policy values with a documented default, not
  physics. They are the standard policy's judgement about one class of facility.
* The freshness rule judges a producer's clock against this runtime's; a persistently
  wrong producer clock shows up as future-dated or stale evidence rather than being
  corrected.
* Timestamps are UTC nanoseconds from the Unix epoch; leap seconds are not
  represented.
* The store is a directory on a local file system. It assumes the file system
  provides atomic rename and durable flush, which is what the operating system's own
  primitives provide.
* No telemetry, no network service, and no remote replication.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
