#pragma once

#include <expected>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "tinyio/logging.hpp"

namespace tinyio {
// A deliberately small error value. `operation` is diagnostic context and
// must have static storage duration; use a string literal.
struct Error {
    std::error_code code{};
    const char* operation = "";

    /// Build an error from a positive errno value (POSIX category).
    [[nodiscard]] static Error FromErrno(const int value, const char* operation = "") noexcept {
        return {.code = {value, std::generic_category()},
                .operation = operation != nullptr ? operation : ""};
    }

    /// Build an error from a std::errc value.
    [[nodiscard]] static Error FromErrc(const std::errc value,
                                        const char* operation = "") noexcept {
        return {.code = std::make_error_code(value),
                .operation = operation != nullptr ? operation : ""};
    }

    /// Build a failed Result from a std::errc value.
    [[nodiscard]] static std::unexpected<Error> FailErrc(const std::errc value,
                                                         const char* operation = "") noexcept {
        return std::unexpected(FromErrc(value, operation));
    }

    /// Build a failed Result from a positive errno value.
    [[nodiscard]] static std::unexpected<Error> FailErrno(const int code,
                                                          const char* operation = "") noexcept {
        return std::unexpected(FromErrno(code, operation));
    }

    /// True when this object carries a failure.
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(code); }

    /// The raw error value: the errno for FromErrno, the errc value for
    /// FromErrc.
    [[nodiscard]] int Value() const noexcept { return code.value(); }

    /// The system message for this error.
    [[nodiscard]] std::string Message() const { return code.message(); }

    /// The diagnostic context ("operation") this error was raised with.
    [[nodiscard]] std::string_view Context() const noexcept {
        return operation != nullptr ? operation : "";
    }

    [[nodiscard]] bool operator==(const std::error_code& rhs) const noexcept { return code == rhs; }

    // errc comparison without spelling make_error_code at every call site:
    // if (res.error() == std::errc::timed_out).
    [[nodiscard]] bool operator==(const std::errc e) const noexcept {
        return code == std::make_error_code(e);
    }

    [[nodiscard]] bool operator==(const Error& rhs) const noexcept { return code == rhs.code; }
};

template <typename T>
using Result = std::expected<T, Error>;

/// Stream an error as "<message> (<value>) [<context>]".
inline std::ostream& operator<<(std::ostream& os, const Error& err) {
    return os << err.Message() << " (" << err.Value() << ") [" << err.Context() << "]";
}
}  // namespace tinyio

template <>
struct std::formatter<tinyio::Error> : std::formatter<std::string> {
    template <typename FormatContext>
    auto format(const tinyio::Error& err, FormatContext& ctx) const {
        return std::formatter<std::string>::format(
            std::format("{} ({}) [{}]", err.Message(), err.Value(), err.Context()), ctx);
    }
};

namespace tinyio::detail {

/// Move the value out of an expected; yields nothing when it holds void.
template <class Exp>
auto UnwrapExpected(Exp&& exp) noexcept {
    using ValueT = std::decay_t<Exp>::value_type;
    if constexpr (std::is_void_v<ValueT>) {
        return;
    } else {
        return std::move(*std::forward<Exp>(exp));
    }
}
}  // namespace tinyio::detail

// ---------------------------------------------------------------------------
// Propagation macros.
//
// These are plain if-statements, not GNU statement expressions: every
// expansion is standard C++ and works under -Wpedantic.
//
// ARGUMENT LIFETIME: pass an RVALUE. The expansion binds auto&& to the
// argument and then moves out of it -- handed an lvalue, the macro silently
// empties the caller's object (the expected keeps has_value() == true with
// a moved-from value). `TRY(n, helper(1));`, not `TRY(n, member_result);`.
// ---------------------------------------------------------------------------

#define TINYIO_DETAIL_CAT_I(a, b) a##b
#define TINYIO_DETAIL_CAT(a, b) TINYIO_DETAIL_CAT_I(a, b)

/// \brief Bind the value of a Result and propagate the error on failure.
///
/// \code
///   TRY(n, io.Read(fd, buf));   // n is the unwrapped value
///   TRY_VOID(io.Close(fd));     // Result<void>: just propagate
/// \endcode
#define TRY(var, ...) TRY_IMPL(var, __COUNTER__, __VA_ARGS__)

#define TRY_IMPL(var, id, ...)                                                          \
    auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (__VA_ARGS__);                         \
    if (!TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]]                  \
        return std::unexpected(std::move(TINYIO_DETAIL_CAT(_tinyio_try_, id)).error()); \
    auto var = ::tinyio::detail::UnwrapExpected(std::move(TINYIO_DETAIL_CAT(_tinyio_try_, id)))

/// \brief Propagate the error of a Result<void>; binds nothing.
///
/// A Result<void> cannot be bound to a variable, so every void operation
/// (close, fsync, drain, ...) needs this form.
#define TRY_VOID(...) TRY_VOID_IMPL(__COUNTER__, __VA_ARGS__)

#define TRY_VOID_IMPL(id, ...)                                         \
    if (auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (__VA_ARGS__);    \
        !TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]] \
    return std::unexpected(std::move(TINYIO_DETAIL_CAT(_tinyio_try_, id)).error())

/// \brief Like \c TRY but logs the error before propagating it.
///
/// \param var The variable the value is bound to.
/// \param expr A \c Result<T>.
/// \param fmt  A format string for the context of the failure; extra
///              arguments are appended to it.
#define TRY_LOG_ERR(var, expr, fmt, ...) TRY_LOG_ERR_IMPL(var, __COUNTER__, expr, fmt, __VA_ARGS__)

#define TRY_LOG_ERR_IMPL(var, id, expr, fmt, ...)                              \
    auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (expr);                       \
    if (!TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]] {       \
        const auto& _tinyio_err = TINYIO_DETAIL_CAT(_tinyio_try_, id).error(); \
        ELOGFMT(ERROR, "{} failed: code={} " fmt, _tinyio_err.Context(),       \
                _tinyio_err.Value() __VA_OPT__(, ) __VA_ARGS__);               \
        return std::unexpected(_tinyio_err);                                   \
    }                                                                          \
    auto var = ::tinyio::detail::UnwrapExpected(std::move(TINYIO_DETAIL_CAT(_tinyio_try_, id)))

/// \brief Like \c TRY_VOID but logs the error before propagating it.
#define TRY_LOG_ERR_VOID(expr, fmt, ...) TRY_LOG_ERR_VOID_IMPL(__COUNTER__, expr, fmt, __VA_ARGS__)

#define TRY_LOG_ERR_VOID_IMPL(id, expr, fmt, ...)                              \
    if (auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (expr);                   \
        !TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]] {       \
        const auto& _tinyio_err = TINYIO_DETAIL_CAT(_tinyio_try_, id).error(); \
        ELOGFMT(ERROR, "{} failed: code={} " fmt, _tinyio_err.Context(),       \
                _tinyio_err.Value() __VA_OPT__(, ) __VA_ARGS__);               \
        return std::unexpected(_tinyio_err);                                   \
    }
