#pragma once

#include <miare/error.hpp>

#include <type_traits>
#include <utility>
#include <variant>

namespace miare {

struct AuthenticationFailed {
    friend bool operator==(AuthenticationFailed, AuthenticationFailed) noexcept = default;
};

struct WriterBusy {
    friend bool operator==(WriterBusy, WriterBusy) noexcept = default;
};

template<class T, class E>
class Result {
    static_assert(!std::is_void_v<T>, "Result<void, E> is not part of the v1 contract");
    static_assert(!std::is_same_v<T, E>, "Result alternatives must have distinct types");

public:
    [[nodiscard]] static Result success(T value) {
        return Result{std::in_place_index<0>, std::move(value)};
    }

    [[nodiscard]] static Result failure(E error) {
        return Result{std::in_place_index<1>, std::move(error)};
    }

    [[nodiscard]] bool hasValue() const noexcept { return value_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    T& value() & {
        requireAlternative(0);
        return std::get<0>(value_);
    }

    const T& value() const& {
        requireAlternative(0);
        return std::get<0>(value_);
    }

    T&& value() && {
        requireAlternative(0);
        return std::get<0>(std::move(value_));
    }

    E& error() & {
        requireAlternative(1);
        return std::get<1>(value_);
    }

    const E& error() const& {
        requireAlternative(1);
        return std::get<1>(value_);
    }

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
