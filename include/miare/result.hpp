#pragma once

#include <miare/error.hpp>

#include <type_traits>
#include <utility>
#include <variant>

namespace miare {

/// Detail-free alternative returned when an encrypted bootstrap cannot authenticate.
struct AuthenticationFailed {
    /// All authentication-failure tags compare equal.
    friend bool operator==(AuthenticationFailed, AuthenticationFailed) noexcept = default;
};

/// Detail-free alternative returned when fair writer admission is unavailable.
struct WriterBusy {
    /// All writer-busy tags compare equal.
    friend bool operator==(WriterBusy, WriterBusy) noexcept = default;
};

/// A value or one explicitly modeled, non-exceptional alternative outcome.
///
/// `Result` does not transport `Errc` values and does not allocate by itself.
/// Accessing the inactive alternative throws `ContractError` with
/// `Errc::InvalidState`.
template<class T, class E>
requires(!std::is_same_v<std::remove_cv_t<T>, Errc> &&
         !std::is_same_v<std::remove_cv_t<E>, Errc>)
class Result {
    static_assert(!std::is_void_v<T>, "Result<void, E> is not part of the v1 contract");

public:
    /// Copies a result when both alternatives are copyable.
    Result(const Result&) = default;
    /// Moves a result and its active alternative.
    Result(Result&&) = default;
    Result& operator=(const Result&) = delete;
    Result& operator=(Result&&) = delete;

    /// Constructs a result holding `value`.
    [[nodiscard]] static Result success(T value) {
        return Result{std::in_place_index<0>, std::move(value)};
    }

    /// Constructs a result holding `error`.
    [[nodiscard]] static Result failure(E error) {
        return Result{std::in_place_index<1>, std::move(error)};
    }

    /// Returns whether this result holds its success value.
    [[nodiscard]] bool hasValue() const noexcept { return value_.index() == 0; }
    /// Returns `true` exactly when this result holds its success value.
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    /// Returns the held success value.
    T& value() & {
        requireAlternative(0);
        return std::get<0>(value_);
    }

    /// Returns the held success value.
    const T& value() const& {
        requireAlternative(0);
        return std::get<0>(value_);
    }

    /// Moves the held success value out of an rvalue result.
    T&& value() && {
        requireAlternative(0);
        return std::get<0>(std::move(value_));
    }

    /// Returns the held alternative error.
    E& error() & {
        requireAlternative(1);
        return std::get<1>(value_);
    }

    /// Returns the held alternative error.
    const E& error() const& {
        requireAlternative(1);
        return std::get<1>(value_);
    }

    /// Moves the held alternative error out of an rvalue result.
    E&& error() && {
        requireAlternative(1);
        return std::get<1>(std::move(value_));
    }

private:
    template<std::size_t Index, class U>
    explicit Result(std::in_place_index_t<Index> index, U&& value)
        : value_(index, std::forward<U>(value)) {}

    void requireAlternative(std::size_t index) const {
        if (value_.index() != index) {
            throw ContractError{Errc::InvalidState, "inactive Result alternative"};
        }
    }

    std::variant<T, E> value_;
};

} // namespace miare
