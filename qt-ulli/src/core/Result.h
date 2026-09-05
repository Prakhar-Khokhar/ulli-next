// core/Result.h
//
// A value-typed Result<T, E> for platform operation outcomes. Forces
// every caller to handle the failure path explicitly (no exceptions in
// installer code — partial disk state is hard to reason about with
// stack unwinding).
//
//   Result<int> r = readCount();
//   if (!r) { log(r.error().message); return; }
//   int n = r.value();
//
// Two kinds of error are produced by the platform backends:
//   * PlatformError — a CLI tool failed (exit code, signal, missing binary)
//   * CancelledError — the user clicked Cancel mid-operation
//
// Use makeOk() / makeError() helpers to construct.

#pragma once

#include <QString>

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ulli::core {

class Error {
public:
    enum class Kind {
        Platform,    // a tool (parted, bcdedit, mkfs.*) returned an error
        Cancelled,   // user requested cancellation
        NotElevated, // admin/root required but not present
        InvalidInput,// user-supplied value failed validation
        Internal,    // bug — an invariant was violated
        NotFound,    // a required file/device/path was not found
        Io,          // filesystem I/O error
    };

    Error(Kind k, std::string msg)
        : kind_(k), message_(std::move(msg)) {}

    Kind kind() const noexcept { return kind_; }
    const std::string& message() const noexcept { return message_; }
    QString qmessage() const { return QString::fromStdString(message_); }

private:
    Kind kind_;
    std::string message_;
};

namespace detail {
    struct OkTag {};
    struct ErrTag {};
}  // namespace detail

template <typename T>
class Result {
public:
    // Success
    Result(detail::OkTag, T value) : data_(std::move(value)) {}
    // Failure
    Result(detail::ErrTag, Error err) : data_(std::move(err)) {}

    bool isOk() const noexcept { return std::holds_alternative<T>(data_); }
    bool isError() const noexcept { return std::holds_alternative<Error>(data_); }
    explicit operator bool() const noexcept { return isOk(); }

    const T& value() const& { return std::get<T>(data_); }
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::move(std::get<T>(data_)); }

    const Error& error() const& { return std::get<Error>(data_); }
    Error error() && { return std::move(std::get<Error>(data_)); }

    // Maps a Result<T,E> to Result<U,E> by applying f to the success value.
    template <typename F>
    auto map(F&& f) const& -> Result<std::invoke_result_t<F, const T&>> {
        using U = std::invoke_result_t<F, const T&>;
        if (isOk()) return Result<U>(detail::OkTag{}, f(value()));
        return Result<U>(detail::ErrTag{}, error());
    }

    // Returns *this on success, otherwise the result of calling f with the
    // error. Lets the caller convert / log / swallow errors in one place.
    template <typename F>
    Result<T> onError(F&& f) const& {
        if (isError()) f(error());
        return *this;
    }

private:
    std::variant<T, Error> data_;
};

// void specialization
template <>
class Result<void> {
public:
    Result(detail::OkTag) : error_() {}
    Result(detail::ErrTag, Error err) : error_(std::move(err)) {}

    bool isOk() const noexcept { return !error_.has_value(); }
    bool isError() const noexcept { return error_.has_value(); }
    explicit operator bool() const noexcept { return isOk(); }

    const Error& error() const& { return *error_; }
    Error error() && { return std::move(*error_); }

    template <typename F>
    Result<void> onError(F&& f) const& {
        if (isError()) f(*error_);
        return *this;
    }

private:
    std::optional<Error> error_;
};

// Deduction guides
template <typename T>
Result(T) -> Result<T>;

// Helpers
template <typename T>
Result<T> makeOk(T value) {
    return Result<T>(detail::OkTag{}, std::move(value));
}

inline Result<void> makeOk() {
    return Result<void>(detail::OkTag{});
}

template <typename T = void>
Result<T> makeError(Error::Kind k, std::string msg) {
    if constexpr (std::is_void_v<T>) {
        return Result<void>(detail::ErrTag{}, Error{k, std::move(msg)});
    } else {
        return Result<T>(detail::ErrTag{}, Error{k, std::move(msg)});
    }
}

template <typename T = void>
Result<T> makeCancelled() {
    return makeError<T>(Error::Kind::Cancelled, "Cancelled by user");
}

template <typename T = void>
Result<T> makeNotElevated() {
    return makeError<T>(Error::Kind::NotElevated,
                        "Administrator/root privileges are required");
}

}  // namespace ulli::core
