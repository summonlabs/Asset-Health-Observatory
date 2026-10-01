// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Outcome<T> and Error: the explicit success-or-failure return type used by
// every fallible Asset Health Observatory operation. No exception crosses the
// public API boundary and no sentinel "invalid" value stands in for absence.

#ifndef ASSET_HEALTH_ERROR_HPP
#define ASSET_HEALTH_ERROR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(_WIN32) && defined(ASSET_HEALTH_SHARED)
#if defined(ASSET_HEALTH_BUILDING_LIBRARY)
#define ASSET_HEALTH_API __declspec(dllexport)
#else
#define ASSET_HEALTH_API __declspec(dllimport)
#endif
#else
#define ASSET_HEALTH_API
#endif

namespace asset_health {

/// Machine-readable rejection category.
///
/// The numeric values are part of the stable public contract: they appear in
/// durable records, in exported documents and in CLI output, so a category is
/// never renumbered and never reused. New categories are appended.
enum class ErrorCode : std::uint16_t {
    None = 0,

    // --- Input syntax and domain validation (1..29) -----------------------
    InvalidInput = 1,
    MalformedEvidenceId = 2,
    MalformedAssetReference = 3,
    MalformedSourceId = 4,
    MalformedComponentId = 5,
    MalformedToken = 6,
    MalformedVersionText = 7,
    MalformedText = 8,
    MalformedTimestamp = 9,
    MalformedQuantity = 10,
    MalformedDuration = 11,
    EmptyRequiredField = 12,
    TextTooLong = 13,
    UnknownMetric = 14,
    UnknownUnit = 15,
    UnknownEvidenceKind = 16,
    UnitMismatch = 17,
    ValueOutOfRange = 18,
    InvalidTimeOrder = 19,
    ArithmeticOverflow = 20,
    InconsistentPayload = 21,
    UnsupportedEvidence = 22,
    InvalidPolicy = 23,
    InvalidLimit = 24,
    InvalidConfiguration = 25,
    InvalidRational = 26,
    DivisionByZero = 27,
    AuthorityDomainViolation = 28,
    SyntheticProvenanceRequired = 29,

    // --- Evidence admission (30..48) --------------------------------------
    DuplicateEvidenceId = 30,
    EvidenceConflict = 31,
    StaleSourceEpoch = 32,
    ReplayedSourceSequence = 33,
    FutureDatedObservation = 34,
    EvidenceCapacityExceeded = 35,
    UnknownAsset = 36,
    UnknownGeneration = 37,
    SupersededGeneration = 38,
    EvidenceNotAdmitted = 39,
    EvidenceRejected = 40,
    AssetCapacityExceeded = 41,
    SourceCapacityExceeded = 42,
    HistoryCapacityExceeded = 43,
    FindingCapacityExceeded = 44,
    PayloadTooLarge = 45,
    InvalidEvidenceBinding = 46,
    AssessmentUnavailable = 47,
    NoEvidence = 48,

    // --- Persistence (49..72) ---------------------------------------------
    StoreOpenFailed = 49,
    StoreNotFound = 50,
    StoreCorrupt = 51,
    StoreVersionUnsupported = 52,
    StoreIntegrityFailed = 53,
    StoreLocked = 54,
    StoreIoError = 55,
    StoreLayoutInvalid = 56,
    StoreRecoveryImpossible = 57,
    StorePublishFailed = 58,
    StoreClosed = 59,
    StoreAlreadyOpen = 60,
    TornTailDiscarded = 61,
    InteriorCorruption = 62,
    StaleStoreEpoch = 63,
    StaleMutationSequence = 64,
    IdempotencyConflict = 65,
    CommitPointMissing = 66,
    StoreReadOnly = 67,
    UnsupportedFormatVersion = 68,
    PayloadChecksumMismatch = 69,
    HeaderChecksumMismatch = 70,
    RecordDecodeFailed = 71,
    RecoveredStateNotCurrent = 72,

    // --- Concurrency and lifecycle (73..84) -------------------------------
    Cancelled = 73,
    ShuttingDown = 74,
    AlreadyClosed = 75,
    QueueFull = 76,
    TicketUnknown = 77,
    WorkerFailure = 78,
    DeadlockDetected = 79,
    ConcurrentMutation = 80,
    PublicationRace = 81,
    TimeoutNotSupported = 82,
    InvalidState = 83,
    OperationRefused = 84,

    // --- Internal (85..) --------------------------------------------------
    InternalInvariantViolation = 85,
    /// The platform could not supply a resource the operation required, such as
    /// entropy for an identifier. Reported rather than worked around, because a
    /// predictable identifier is a worse outcome than a refusal.
    AllocationFailed = 86,
};

/// Number of error codes, so a caller can iterate the whole set.
inline constexpr std::size_t kErrorCodeCount = 87;

/// Stable machine-readable name of an error code (lower snake case).
[[nodiscard]] ASSET_HEALTH_API std::string_view error_code_name(ErrorCode code) noexcept;

/// Parses the stable name produced by error_code_name. Returns std::nullopt for
/// an unknown name; an unknown name is never treated as a valid code.
[[nodiscard]] ASSET_HEALTH_API std::optional<ErrorCode> error_code_from_name(std::string_view name) noexcept;

/// True when the code says untrusted input failed validation.
[[nodiscard]] ASSET_HEALTH_API bool is_input_error(ErrorCode code) noexcept;

/// True when the code says the caller's view was out of date. Such a rejection
/// is always safe to retry after re-reading.
[[nodiscard]] ASSET_HEALTH_API bool is_staleness_error(ErrorCode code) noexcept;

/// True when the code says durable state could not be trusted.
[[nodiscard]] ASSET_HEALTH_API bool is_storage_error(ErrorCode code) noexcept;

/// True when the code says a configured bound was reached.
[[nodiscard]] ASSET_HEALTH_API bool is_capacity_error(ErrorCode code) noexcept;

/// True when the code says the evidence shown does not carry the authority the
/// caller assumed. This Observatory reports such a rejection rather than
/// inferring the authority from the evidence it can see.
[[nodiscard]] ASSET_HEALTH_API bool is_boundary_error(ErrorCode code) noexcept;

/// A rejection as a value: a stable code, one concise human explanation, and an
/// optional structured context pair. Cheap to copy; never owns observatory state.
class ASSET_HEALTH_API Error {
public:
    Error() = default;
    Error(ErrorCode code, std::string message);

    /// Attaches a structured context pair, e.g. ("generation", "7").
    [[nodiscard]] Error with_context(std::string key, std::string value) const;

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::None; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::optional<std::pair<std::string, std::string>>& context() const noexcept {
        return context_;
    }

    /// "code: message (key=value)" in a form stable enough to assert on.
    [[nodiscard]] std::string to_string() const;

private:
    ErrorCode code_ = ErrorCode::None;
    std::string message_;
    std::optional<std::pair<std::string, std::string>> context_;
};

/// The explicit success-or-failure result of a fallible operation.
template <typename T>
class [[nodiscard]] Outcome {
public:
    Outcome(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Outcome(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }

    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }

    /// Member access on the held value, as std::optional offers. A caller that
    /// reaches through a rejection has a defect of its own; this returns null
    /// rather than pretending a value exists.
    [[nodiscard]] const T* operator->() const noexcept { return value_.has_value() ? &*value_ : nullptr; }
    [[nodiscard]] T* operator->() noexcept { return value_.has_value() ? &*value_ : nullptr; }

    [[nodiscard]] const Error& error() const noexcept { return error_; }

    /// Returns the held value, or p fallback when this outcome is a rejection.
    [[nodiscard]] T value_or(T fallback) const {
        return value_.has_value() ? *value_ : std::move(fallback);
    }

    /// Applies p fn to the held value, propagating a rejection unchanged.
    template <typename Fn>
    [[nodiscard]] auto map(Fn&& fn) const -> Outcome<std::invoke_result_t<Fn, const T&>> {
        using U = std::invoke_result_t<Fn, const T&>;
        if (!value_.has_value()) {
            return error_;
        }
        return static_cast<U>(std::forward<Fn>(fn)(*value_));
    }

private:
    std::optional<T> value_;
    Error error_;
};

/// The void specialisation: a success carries nothing, a failure carries the reason.
template <>
class [[nodiscard]] Outcome<void> {
public:
    Outcome() = default;
    Outcome(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return error_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return error_.ok(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    Error error_;
};

using Status = Outcome<void>;

[[nodiscard]] inline Status ok() noexcept { return Status{}; }

}  // namespace asset_health

#endif  // ASSET_HEALTH_ERROR_HPP
