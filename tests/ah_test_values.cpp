// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Values: strong types, canonical spellings, exact arithmetic, time, and the
// policy. A defect here would be a wrong answer everywhere else, so every
// property is pinned rather than sampled.

#include <cstdint>
#include <string>
#include <vector>

#include "asset_health/export.hpp"
#include "asset_health/policy.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace asset_health;
using namespace asset_test;

}  // namespace

AH_TEST(values, rational_is_exact_and_normalised) {
    const auto third = Rational::make(1, 3);
    AH_REQUIRE(third.has_value());
    AH_CHECK(third->numerator() == 1);
    AH_CHECK(third->denominator() == 3);

    const auto two_sixths = Rational::make(2, 6);
    AH_REQUIRE(two_sixths.has_value());
    AH_CHECK(*two_sixths == *third);

    const auto negative = Rational::make(1, -2);
    AH_REQUIRE(negative.has_value());
    AH_CHECK(negative->numerator() == -1);
    AH_CHECK(negative->denominator() == 2);

    AH_CHECK(!Rational::make(1, 0).has_value());
    AH_CHECK(Rational::make(0, 7)->denominator() == 1);
}

AH_TEST(values, rational_arithmetic_compares_exactly) {
    const Rational half = Rational::from_integer(1).divide(Rational::from_integer(2)).value();
    const Rational third = Rational::make(1, 3).value();
    const Rational sum = half.add(third).value();
    AH_CHECK(sum == Rational::make(5, 6).value());
    AH_CHECK(third < half);
    AH_CHECK(half.subtract(third).value() == Rational::make(1, 6).value());
    AH_CHECK(half.multiply(third).value() == Rational::make(1, 6).value());
    AH_CHECK(half.to_decimal_string(4) == "0.5000");
    AH_CHECK(Rational::make(-1, 4).value().to_decimal_string(2) == "-0.25");

    // The comparison must not overflow where a cross multiplication would.
    const Rational large = Rational::make(4611686018427387904LL, 3).value();
    const Rational small = Rational::make(1, 4611686018427387904LL).value();
    AH_CHECK(small < large);
}

AH_TEST(values, tokens_reject_every_non_canonical_spelling) {
    AH_CHECK(Token::parse("standard-dccp-health").has_value());
    AH_CHECK(Token::parse("gpu-0").has_value());
    AH_CHECK(Token::parse("a.b_c-1").has_value());
    AH_CHECK(!Token::parse("").has_value());
    AH_CHECK(!Token::parse("Upper").has_value());
    AH_CHECK(!Token::parse("-leading").has_value());
    AH_CHECK(!Token::parse("trailing-").has_value());
    AH_CHECK(!Token::parse("double--separator").has_value());
    AH_CHECK(!Token::parse("has space").has_value());
    AH_CHECK(!Token::parse(std::string(65, 'a')).has_value());
}

AH_TEST(values, identifiers_round_trip_and_reject_other_forms) {
    const std::string text = "01234567-89ab-4cde-8f01-23456789abcd";
    const auto parsed = AssetRefId::parse(text);
    AH_REQUIRE(parsed.has_value());
    AH_CHECK(parsed->to_string() == text);
    AH_CHECK(parsed->to_compact_string() == "0123456789ab4cde8f0123456789abcd");
    AH_CHECK(!AssetRefId::parse("0123456789ab4cde8f0123456789abcd").has_value());
    AH_CHECK(!AssetRefId::parse("01234567-89AB-4cde-8f01-23456789abcd").has_value());
    AH_CHECK(!AssetRefId::parse("{01234567-89ab-4cde-8f01-23456789abcd}").has_value());
    AH_CHECK(!AssetRefId::parse("urn:uuid:01234567-89ab-4cde-8f01-23456789abcd").has_value());
    AH_CHECK(AssetRefId::nil().is_nil());

    const auto generated = EvidenceId::generate();
    AH_REQUIRE(generated.has_value());
    AH_CHECK(is_uuid_v4(generated->bytes()));
    const auto reparsed = EvidenceId::parse(generated->to_string());
    AH_REQUIRE(reparsed.has_value());
    AH_CHECK(*reparsed == generated.value());
}

AH_TEST(values, instant_and_duration_round_trip) {
    const auto parsed = parse_instant("2026-07-04T12:34:56.789Z");
    AH_REQUIRE(parsed.has_value());
    AH_CHECK(parsed->to_string() == "2026-07-04T12:34:56.789000000Z");
    AH_CHECK(parsed->to_millisecond_string() == "2026-07-04T12:34:56.789Z");
    AH_CHECK(parse_instant("2026-07-04T12:34:56Z").has_value());
    AH_CHECK(parse_instant("2026-07-04T12:34:56+00:00").has_value());
    AH_CHECK(!parse_instant("2026-07-04T12:34:56+01:00").has_value());
    AH_CHECK(!parse_instant("2026-13-04T12:34:56Z").has_value());
    AH_CHECK(!parse_instant("2026-02-30T00:00:00Z").has_value());
    AH_CHECK(!parse_instant("2026-07-04 12:34:56Z").has_value());
    AH_CHECK(parse_instant("2024-02-29T00:00:00Z").has_value());
    AH_CHECK(!parse_instant("2026-02-29T00:00:00Z").has_value());

    const auto duration = parse_duration("90s");
    AH_REQUIRE(duration.has_value());
    AH_CHECK(duration->seconds() == 90);
    AH_CHECK(parse_duration("-1.5h")->hours() == -1);
    AH_CHECK(!parse_duration("90").has_value());
    AH_CHECK(!parse_duration("").has_value());

    const auto later = parsed->shifted(Duration::from_hours(25));
    AH_REQUIRE(later.has_value());
    AH_CHECK(later->to_string() == "2026-07-05T13:34:56.789000000Z");
    AH_CHECK(later->minus(*parsed) == Duration::from_hours(25));
}

AH_TEST(values, quantity_is_fixed_point_and_round_trips) {
    const auto value = Quantity::parse("42.5 c");
    AH_REQUIRE(value.has_value());
    AH_CHECK(value->milli() == 42500);
    AH_CHECK(value->to_string() == "42.500 c");
    AH_CHECK(Quantity::parse("42.5c")->milli() == 42500);
    AH_CHECK(Quantity::parse("-3 rpm")->milli() == -3000);
    AH_CHECK(!Quantity::parse("42.5001 c").has_value());
    AH_CHECK(!Quantity::parse("42").has_value());
    AH_CHECK(!Quantity::parse("42 xyz").has_value());
    AH_CHECK(Quantity::make(Unit::Celsius, 1000)->whole() == 1);
    const auto difference = Quantity::make(Unit::Celsius, 1000)->difference_milli(*Quantity::make(Unit::Celsius, 2500));
    AH_REQUIRE(difference.has_value());
    AH_CHECK(*difference == -1500);
    AH_CHECK(!Quantity::make(Unit::Celsius, 1)->difference_milli(*Quantity::make(Unit::Watts, 1)).has_value());
    AH_CHECK(Quantity::make(Unit::Celsius, 5)->absolute_difference_milli(*Quantity::make(Unit::Celsius, -7)) == 12);
}

AH_TEST(values, metrics_declare_their_unit_direction_and_semantics) {
    for (std::size_t index = 0; index < kMetricKindCount; ++index) {
        const auto metric = static_cast<MetricKind>(index);
        AH_CHECK(canonical_unit(metric) != Unit::None);
        AH_CHECK(!std::string(to_string(metric)).empty());
        AH_CHECK(parse_metric(to_string(metric)) == metric);
    }
    AH_CHECK(canonical_unit(MetricKind::Temperature) == Unit::Celsius);
    AH_CHECK(canonical_unit(MetricKind::Uptime) == Unit::Hours);
    AH_CHECK(adverse_direction(MetricKind::Temperature) == AdverseDirection::Higher);
    AH_CHECK(adverse_direction(MetricKind::FanSpeed) == AdverseDirection::Lower);
    AH_CHECK(adverse_direction(MetricKind::Voltage) == AdverseDirection::TwoSided);
    AH_CHECK(metric_semantics(MetricKind::Uptime) == MetricSemantics::Counter);
    AH_CHECK(metric_semantics(MetricKind::Temperature) == MetricSemantics::Gauge);
}

AH_TEST(values, lifecycle_vocabulary_matches_the_lifecycle_authority) {
    for (std::size_t index = 0; index < kLifecycleStateCount; ++index) {
        const auto state = static_cast<LifecycleState>(index);
        AH_CHECK(parse_lifecycle_state(to_string(state)) == state);
    }
    AH_CHECK(to_string(LifecycleState::Commissioning) == "commissioning");
    AH_CHECK(to_string(LifecycleState::Quarantined) == "quarantined");
    AH_CHECK(is_in_service(LifecycleState::Active));
    AH_CHECK(is_in_service(LifecycleState::Maintenance));
    AH_CHECK(!is_in_service(LifecycleState::Installed));
    AH_CHECK(is_end_of_life(LifecycleState::Retired));
    AH_CHECK(is_end_of_life(LifecycleState::Removed));
    AH_CHECK(!is_end_of_life(LifecycleState::Retiring));
    AH_CHECK(service_class(LifecycleState::Degraded) == ServiceClass::InServiceImpaired);
}

AH_TEST(values, error_codes_round_trip_through_their_names) {
    for (std::uint16_t index = 0; index < kErrorCodeCount; ++index) {
        const auto code = static_cast<ErrorCode>(index);
        const std::string_view name = error_code_name(code);
        AH_CHECK(!name.empty());
        AH_CHECK(name != "unknown_code");
        AH_CHECK(error_code_from_name(name) == code);
    }
    AH_CHECK(!error_code_from_name("no_such_code").has_value());
    AH_CHECK(is_storage_error(ErrorCode::StoreCorrupt));
    AH_CHECK(is_input_error(ErrorCode::MalformedToken));
    AH_CHECK(is_capacity_error(ErrorCode::EvidenceCapacityExceeded));
    AH_CHECK(is_boundary_error(ErrorCode::AuthorityDomainViolation));

    const Error error(ErrorCode::InvalidPolicy, "thresholds are not ordered");
    AH_CHECK(error.to_string() == "invalid_policy: thresholds are not ordered");
    AH_CHECK(error.with_context("metric", "temperature").to_string() ==
             "invalid_policy: thresholds are not ordered (metric=temperature)");
}

AH_TEST(values, standard_policy_is_valid_stable_and_fingerprinted) {
    const HealthPolicy& policy = HealthPolicy::standard();
    AH_CHECK(policy.id().str() == "standard-dccp-health");
    AH_CHECK(policy.version().value() == 1);
    AH_CHECK(policy.fingerprint().size() == 16);
    AH_CHECK(policy.metric(MetricKind::Temperature).required);
    AH_CHECK(!policy.metric(MetricKind::Uptime).required);
    AH_CHECK(policy.fault_impact(FaultSeverity::Critical) == HealthState::Failed);
    AH_CHECK(policy.unknown_lifecycle_cap() == HealthState::Unknown);

    // The fingerprint is a function of the canonical text, so rebuilding the same
    // policy from the same values reproduces it, and changing one threshold does
    // not.
    HealthPolicy::Parts parts;
    parts.id = *Token::parse("standard-dccp-health");
    parts.version = PolicyVersion(1);
    parts.freshness.dynamic_window = Duration::from_seconds(300);
    parts.freshness.static_window = Duration::from_days(30);
    parts.freshness.future_tolerance = Duration::from_seconds(5);
    parts.metrics = policy.metrics();
    parts.fault_impact = policy.fault_impacts();
    parts.degradation = policy.degradation();
    parts.maintenance = policy.maintenance();
    parts.risk = policy.risk();
    parts.clock_skew_tolerance = policy.clock_skew_tolerance();
    const auto rebuilt = HealthPolicy::make(parts);
    AH_REQUIRE(rebuilt.has_value());
    AH_CHECK(rebuilt->fingerprint() == policy.fingerprint());

    parts.metrics[static_cast<std::size_t>(MetricKind::Temperature)].warn_high = 70000;
    const auto changed = HealthPolicy::make(parts);
    AH_REQUIRE(changed.has_value());
    AH_CHECK(changed->fingerprint() != policy.fingerprint());
}

AH_TEST(values, policy_rejects_incoherent_configuration) {
    HealthPolicy::Parts parts;
    parts.id = *Token::parse("test-policy");
    parts.version = PolicyVersion(1);

    const auto simple = HealthPolicy::make(parts);
    AH_REQUIRE(simple.has_value());
    AH_CHECK(simple->fingerprint().size() == 16);

    HealthPolicy::Parts missing_id;
    missing_id.version = PolicyVersion(1);
    AH_CHECK(!HealthPolicy::make(missing_id).has_value());

    HealthPolicy::Parts zero_version;
    zero_version.id = *Token::parse("test-policy");
    AH_CHECK(!HealthPolicy::make(zero_version).has_value());

    HealthPolicy::Parts unordered;
    unordered.id = *Token::parse("test-policy");
    unordered.version = PolicyVersion(1);
    unordered.metrics[static_cast<std::size_t>(MetricKind::Temperature)].warn_high = 90000;
    unordered.metrics[static_cast<std::size_t>(MetricKind::Temperature)].critical_high = 80000;
    AH_CHECK(!HealthPolicy::make(unordered).has_value());

    HealthPolicy::Parts zero_weight;
    zero_weight.id = *Token::parse("test-policy");
    zero_weight.version = PolicyVersion(1);
    zero_weight.risk.weight_age = Rational::from_integer(0);
    AH_CHECK(!HealthPolicy::make(zero_weight).has_value());

    HealthPolicy::Parts backwards_bands;
    backwards_bands.id = *Token::parse("test-policy");
    backwards_bands.version = PolicyVersion(1);
    backwards_bands.risk.band_low = Rational::make(3, 4).value();
    backwards_bands.risk.band_moderate = Rational::make(1, 2).value();
    AH_CHECK(!HealthPolicy::make(backwards_bands).has_value());

    HealthPolicy::Parts short_window;
    short_window.id = *Token::parse("test-policy");
    short_window.version = PolicyVersion(1);
    short_window.degradation.min_samples = 1;
    AH_CHECK(!HealthPolicy::make(short_window).has_value());
}

AH_TEST(values, json_of_a_policy_is_canonical) {
    const std::string text = to_json(HealthPolicy::standard());
    AH_CHECK(text.find("\"id\": \"standard-dccp-health\"") != std::string::npos);
    AH_CHECK(text.find("\"fingerprint\"") != std::string::npos);
    AH_CHECK(text.back() == '\n');
}
