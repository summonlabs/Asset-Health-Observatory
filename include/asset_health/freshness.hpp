// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Freshness: whether an admitted statement still counts as current evidence.
//
// The rules are one function with no hidden inputs, so "why is this stale" has
// exactly one answer and a test can pin it:
//
//   * an observation dated later than the evaluation time by more than the skew
//     tolerance is future-dated, and a future-dated reading is not evidence;
//   * dynamic evidence (telemetry, faults) is fresh only while its source stream
//     epoch is still the newest one seen, and only inside the same store epoch
//     that committed it unless that epoch was closed cleanly. A session that died
//     therefore demotes everything it had already said -- recovered dynamic
//     evidence is not fresh evidence -- while an orderly restart leaves a reading
//     to be judged by its age, which is what lets one command ingest and a later
//     command assess without the second one discarding the first one's work;
//   * static evidence (identity, lifecycle, maintenance, firmware) describes a
//     state that persists until it is superseded, so it is judged by age alone,
//     and a recovered static statement is reported as recovered while still
//     being usable.

#ifndef ASSET_HEALTH_FRESHNESS_HPP
#define ASSET_HEALTH_FRESHNESS_HPP

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "asset_health/error.hpp"
#include "asset_health/evidence.hpp"
#include "asset_health/strong_types.hpp"
#include "asset_health/time.hpp"

namespace asset_health {

/// The answer to "may this statement still count as current".
enum class Freshness : std::uint8_t {
    /// Inside its window, from a live stream, committed in this store epoch.
    Fresh = 0,
    /// Older than its window.
    Stale = 1,
    /// Dated later than the evaluation time by more than the skew tolerance.
    FutureDated = 2,
    /// Dynamic evidence committed in an earlier store epoch: recovered, and
    /// therefore not current.
    Recovered = 3,
    /// The producing stream has restarted, so the statement belongs to a dead
    /// epoch of that stream.
    SupersededEpoch = 4,
    /// Static evidence with no configured window: a state, not a reading.
    Unaged = 5,
};

/// Number of Freshness enumerators.
inline constexpr std::size_t kFreshnessCount = 6;

/// The machine-readable reason behind a freshness answer. Separate from the
/// answer because two different reasons can produce the same answer, and the
/// explanation should say which one applied.
enum class FreshnessReason : std::uint8_t {
    WithinWindow = 0,
    AgeExceedsWindow = 1,
    InPreviousStoreEpoch = 2,
    SourceEpochSuperseded = 3,
    ObservedInFuture = 4,
    StaticStateNotAged = 5,
    StaticStateAged = 6,
};

[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(Freshness freshness) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::string_view to_string(FreshnessReason reason) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<Freshness> parse_freshness(std::string_view text) noexcept;
[[nodiscard]] ASSET_HEALTH_API std::optional<FreshnessReason> parse_freshness_reason(std::string_view text) noexcept;

/// The configured windows. All three are policy data with explicit defaults, and
/// a policy that sets a window to zero means "nothing of this class is ever
/// fresh", not "use the default".
struct FreshnessPolicy {
    /// Age beyond which a telemetry reading or fault statement is stale.
    Duration dynamic_window = Duration::from_seconds(300);
    /// Age beyond which an identity, lifecycle, maintenance or firmware statement
    /// is stale.
    Duration static_window = Duration::from_days(30);
    /// How far an observation may be dated ahead of the receiving clock before it
    /// is future-dated. Zero tolerates nothing.
    Duration future_tolerance = Duration::from_seconds(5);
    /// When true, static evidence is never aged: it stays usable until a newer
    /// statement about the same fact arrives.
    bool static_unaged = false;
};

/// The result of judging one record. Carries the age so a caller can explain the
/// answer without recomputing it.
struct FreshnessResult {
    Freshness state = Freshness::Stale;
    FreshnessReason reason = FreshnessReason::AgeExceedsWindow;
    /// now - observed_at, saturating, never wrapped.
    Duration age{};
    /// True when this statement may contribute to a health conclusion.
    bool usable = false;
    /// One line naming the window, the age and the rule that decided the answer.
    std::string explanation;
};

/// Judges one record.
///
/// \p current_store_epoch is the epoch of the store that is being read.
/// \p newest_source_epochs maps each source to the newest stream epoch this
/// observatory has seen from it; a source that is absent from the map has not
/// been heard from in this store epoch, and its earlier statements are judged on
/// age alone.
/// \p store_recovered says whether the state being read was reconstructed by
/// recovery, that is whether the session that wrote it ended without closing
/// cleanly. It is what separates an orderly restart, after which a reading is
/// still judged by its age, from a session that died, after which its
/// unconfirmed readings are not current evidence at all.
[[nodiscard]] ASSET_HEALTH_API FreshnessResult evaluate_freshness(
    const EvidenceRecord& record, Instant now, const FreshnessPolicy& policy, StoreEpoch current_store_epoch,
    const std::map<SourceId, StreamEpoch>& newest_source_epochs, bool store_recovered);

/// True when a freshness answer means the statement may support a health state.
[[nodiscard]] ASSET_HEALTH_API bool is_usable(Freshness freshness) noexcept;

}  // namespace asset_health

#endif  // ASSET_HEALTH_FRESHNESS_HPP
