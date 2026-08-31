#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace miare {

/// Stable error categories used by public exceptions.
enum class Errc : std::uint16_t {
    /// A caller-supplied argument violates an operation contract.
    InvalidArgument = 0,
    /// Options or compile-time policy values are inconsistent.
    InvalidConfiguration = 1,
    /// A handle or session is not in a state that permits the operation.
    InvalidState = 2,
    /// A thread-affine handle was used from another thread.
    WrongThread = 3,
    /// A parent cannot terminate while subordinate handles remain live.
    LiveChildren = 4,
    /// An operating-system or durable-file I/O operation failed.
    Io = 5,
    /// Stable-storage or namespace durability could not be established.
    Durability = 6,
    /// Authenticated authoritative state violates an integrity invariant.
    Corrupt = 7,
    /// The file envelope or format version cannot be read by this build.
    UnsupportedFormat = 8,
    /// The file requires a recognized but unavailable feature.
    UnsupportedFeature = 9,
    /// The database capacity profile differs from the compiled profile.
    IncompatibleProfile = 10,
    /// A required cryptographic or compression capability failed.
    ProviderUnavailable = 11,
    /// A configured or persistent capacity bound would be exceeded.
    ResourceLimit = 12,
    /// The session cannot continue until it is closed and reopened.
    RecoveryRequired = 13,
    /// A commit failed and is known not to have published.
    CommitFailed = 14,
    /// Reopen recovery must determine whether a commit published.
    CommitOutcomeUnknown = 15,
    /// Another process or session already owns the database file.
    InUse = 16,
};

/// Reports a caller contract violation or invalid handle use.
class ContractError : public std::logic_error {
public:
    /// Constructs a contract error with a stable category and diagnostic text.
    explicit ContractError(Errc code, std::string message)
        : std::logic_error(std::move(message)), code_(code) {}

    /// Returns the stable category suitable for control flow.
    [[nodiscard]] Errc code() const noexcept { return code_; }

private:
    Errc code_;
};

/// Reports an operational failure while fulfilling a database operation.
class DatabaseError : public std::runtime_error {
public:
    /// Constructs an operational error and optional native system error.
    explicit DatabaseError(
        Errc code,
        std::string message,
        std::optional<std::error_code> nativeCode = std::nullopt)
        : std::runtime_error(std::move(message)),
          code_(code),
          nativeCode_(nativeCode) {}

    /// Returns the stable category suitable for control flow.
    [[nodiscard]] Errc code() const noexcept { return code_; }

    /// Returns the underlying system error when one directly caused failure.
    [[nodiscard]] std::optional<std::error_code> nativeCode() const noexcept {
        return nativeCode_;
    }

private:
    Errc code_;
    std::optional<std::error_code> nativeCode_;
};

} // namespace miare
