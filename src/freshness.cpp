// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "asset_health/freshness.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace asset_health {
namespace {

constexpr std::array<std::string_view, kFreshnessCount> kFreshnessNames{{
    "fresh", "stale", "future-dated", "recovered", "superseded-epoch", "unaged",
}};

constexpr std::array<std::string_view, 7> kFreshnessReasonNames{{
    "within-window", "age-exceeds-window", "in-previous-store-epoch", "source-epoch-superseded",
    "observed-in-future", "static-state-not-aged", "static-state-aged",
}};

[[nodiscard]] std::string describe_window(const char* label, Duration window) {
    std::string text = label;
    text.append("=");
    text.append(window.to_seconds_string());
    return text;
}

}  // namespace

std::string_view to_string(Freshness freshness) noexcept {
    const auto index = static_cast<std::size_t>(freshness);
    return index < kFreshnessNames.size() ? kFreshnessNames[index] : std::string_view{"unknown"};
}

std::string_view to_string(FreshnessReason reason) noexcept {
    const auto index = static_cast<std::size_t>(reason);
    return index < kFreshnessReasonNames.size() ? kFreshnessReasonNames[index] : std::string_view{"unknown"};
}

std::optional<Freshness> parse_freshness(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kFreshnessNames.size(); ++index) {
        if (kFreshnessNames[index] == text) {
            return static_cast<Freshness>(index);
        }
    }
    return std::nullopt;
}

std::optional<FreshnessReason> parse_freshness_reason(std::string_view text) noexcept {
    for (std::size_t index = 0; index < kFreshnessReasonNames.size(); ++index) {
        if (kFreshnessReasonNames[index] == text) {
            return static_cast<FreshnessReason>(index);
        }
    }
    return std::nullopt;
}

bool is_usable(Freshness freshness) noexcept {
    return freshness == Freshness::Fresh || freshness == Freshness::Unaged;
}

FreshnessResult evaluate_freshness(const EvidenceRecord& record, Instant now, const FreshnessPolicy& policy,
                                   StoreEpoch current_store_epoch,
                                   const std::map<SourceId, StreamEpoch>& newest_source_epochs,
                                   bool store_recovered) {
    FreshnessResult result;
    result.age = now.minus(record.observed_at());

    // A statement dated ahead of the evaluation instant is not evidence about
    // now: the producer's clock, the evaluation instant, or both are wrong, and
    // guessing which would put an unverifiable claim into the health state.
    if (result.age.is_negative() && result.age.magnitude() > policy.future_tolerance) {
        result.state = Freshness::FutureDated;
        result.reason = FreshnessReason::ObservedInFuture;
        result.usable = false;
        result.explanation = "observed " + result.age.magnitude().to_seconds_string() +
                             " ahead of the evaluation instant, beyond the " +
                             policy.future_tolerance.to_seconds_string() + " skew tolerance";
        return result;
    }

    const FreshnessClass klass = freshness_class_of(record.kind());
    if (klass == FreshnessClass::Dynamic) {
        // The ordering of these two tests is the point of the rule: an epoch
        // check is about ownership, and it is applied before the age check so
        // that a recovered reading is never reported as merely old. The check
        // applies only when the state was reconstructed by recovery: an orderly
        // restart leaves the store's own history intact, and a reading from it is
        // judged by its age.
        if (store_recovered && record.commit_epoch() != current_store_epoch) {
            result.state = Freshness::Recovered;
            result.reason = FreshnessReason::InPreviousStoreEpoch;
            result.usable = false;
            result.explanation = "dynamic evidence committed in store epoch " + record.commit_epoch().to_string() +
                                 ", this store is at epoch " + current_store_epoch.to_string() +
                                 ", and the session that wrote it did not close cleanly; recovered dynamic "
                                 "evidence is not fresh evidence";
            return result;
        }
        const auto stream = newest_source_epochs.find(record.source());
        if (stream != newest_source_epochs.end() && record.provenance().epoch() < stream->second) {
            result.state = Freshness::SupersededEpoch;
            result.reason = FreshnessReason::SourceEpochSuperseded;
            result.usable = false;
            result.explanation = "source " + record.source().str() + " has restarted into stream epoch " +
                                 stream->second.to_string() + "; this statement belongs to epoch " +
                                 record.provenance().epoch().to_string();
            return result;
        }
        if (result.age > policy.dynamic_window) {
            result.state = Freshness::Stale;
            result.reason = FreshnessReason::AgeExceedsWindow;
            result.usable = false;
            result.explanation = "age " + result.age.to_seconds_string() + " exceeds the dynamic window " +
                                 policy.dynamic_window.to_seconds_string();
            return result;
        }
        result.state = Freshness::Fresh;
        result.reason = FreshnessReason::WithinWindow;
        result.usable = true;
        result.explanation = "age " + result.age.to_seconds_string() + " is inside the dynamic window " +
                             policy.dynamic_window.to_seconds_string();
        return result;
    }

    if (policy.static_unaged) {
        result.state = Freshness::Unaged;
        result.reason = FreshnessReason::StaticStateNotAged;
        result.usable = true;
        result.explanation = "static evidence is not aged by this policy; it stands until a newer statement "
                             "about the same fact arrives";
        return result;
    }
    if (result.age > policy.static_window) {
        result.state = Freshness::Stale;
        result.reason = FreshnessReason::StaticStateAged;
        result.usable = false;
        result.explanation = "age " + result.age.to_seconds_string() + " exceeds the static window " +
                             policy.static_window.to_seconds_string();
        return result;
    }
    result.state = Freshness::Fresh;
    result.reason = FreshnessReason::WithinWindow;
    result.usable = true;
    result.explanation = "age " + result.age.to_seconds_string() + " is inside the static window " +
                         policy.static_window.to_seconds_string();
    return result;
}

}  // namespace asset_health
