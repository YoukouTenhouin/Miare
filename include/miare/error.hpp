#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace miare {

enum class Errc : std::uint16_t {
    InvalidArgument = 0,
    InvalidConfiguration = 1,
    InvalidState = 2,
    WrongThread = 3,
    LiveChildren = 4,
    Io = 5,
    Durability = 6,
    Corrupt = 7,
    UnsupportedFormat = 8,
    UnsupportedFeature = 9,
    IncompatibleProfile = 10,
    ProviderUnavailable = 11,
    ResourceLimit = 12,
    RecoveryRequired = 13,
    CommitFailed = 14,
    CommitOutcomeUnknown = 15,
    InUse = 16,
};

class ContractError : public std::logic_error {
public:
    explicit ContractError(Errc code, std::string message)
        : std::logic_error(std::move(message)), code_(code) {}

    [[nodiscard]] Errc code() const noexcept { return code_; }

private:
    Errc code_;
};

class DatabaseError : public std::runtime_error {
public:
    explicit DatabaseError(
        Errc code,
        std::string message,
        std::optional<std::error_code> nativeCode = std::nullopt)
        : std::runtime_error(std::move(message)),
          code_(code),
          nativeCode_(nativeCode) {}

    [[nodiscard]] Errc code() const noexcept { return code_; }

    [[nodiscard]] std::optional<std::error_code> nativeCode() const noexcept {
        return nativeCode_;
    }

private:
    Errc code_;
    std::optional<std::error_code> nativeCode_;
};

} // namespace miare
