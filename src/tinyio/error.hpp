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

/// Extract the value from an expected, moving when given an rvalue and
/// copying when given an lvalue; yields nothing when it holds void.
template <class Exp>
decltype(auto) UnwrapExpected(Exp&& exp) noexcept(
    std::is_void_v<typename std::decay_t<Exp>::value_type> ||
    std::is_nothrow_constructible_v<typename std::decay_t<Exp>::value_type,
                                    decltype(*std::forward<Exp>(exp))>) {
    using ValueT = std::decay_t<Exp>::value_type;
    if constexpr (std::is_void_v<ValueT>) {
        return;
    } else {
        return ValueT(*std::forward<Exp>(exp));
    }
}
}  // namespace tinyio::detail

// ---------------------------------------------------------------------------
// Propagation macros.
//
// These are plain if-statements, not GNU statement expressions: every
// expansion is standard C++ and works under -Wpedantic.
//
// Rvalues are moved out of; lvalues are copied from (or rejected at compile
// time when the value type is move-only, preventing accidental silent moves
// out of an lvalue Result).
// ---------------------------------------------------------------------------

#define TINYIO_DETAIL_CAT_I(a, b) a##b
#define TINYIO_DETAIL_CAT(a, b) TINYIO_DETAIL_CAT_I(a, b)

/// \brief Bind the value of a Result and propagate the error on failure.
///
/// \code
///   TRY(n, io.Read(fd, buf));   // n is the unwrapped value
///   TRY_VOID(io.Close(fd));     // Result<void>: just propagate
/// \endcode
#define TINYIO_TRY(var, ...) TINYIO_TRY_IMPL(var, __COUNTER__, __VA_ARGS__)

#define TINYIO_TRY_IMPL(var, id, ...)                                                       \
    auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (__VA_ARGS__);                             \
    if (!TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]]                      \
        return std::unexpected(std::forward<decltype(TINYIO_DETAIL_CAT(_tinyio_try_, id))>( \
                                   TINYIO_DETAIL_CAT(_tinyio_try_, id))                     \
                                   .error());                                               \
    auto var = ::tinyio::detail::UnwrapExpected(                                            \
        std::forward<decltype(TINYIO_DETAIL_CAT(_tinyio_try_, id))>(                        \
            TINYIO_DETAIL_CAT(_tinyio_try_, id)))

/// \brief Propagate the error of a Result<void>; binds nothing.
///
/// A Result<void> cannot be bound to a variable, so every void operation
/// (close, fsync, drain, ...) needs this form.
#define TINYIO_TRY_VOID(...) TINYIO_TRY_VOID_IMPL(__COUNTER__, __VA_ARGS__)

#define TINYIO_TRY_VOID_IMPL(id, ...)                                                   \
    if (auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (__VA_ARGS__);                     \
        !TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]]                  \
    return std::unexpected(std::forward<decltype(TINYIO_DETAIL_CAT(_tinyio_try_, id))>( \
                               TINYIO_DETAIL_CAT(_tinyio_try_, id))                     \
                               .error())

/// \brief Like \c TINYIO_TRY but logs the error before propagating it.
///
/// \param var The variable the value is bound to.
/// \param expr A \c Result<T>.
/// \param fmt  A format string for the context of the failure; extra
///              arguments are appended to it.
#define TINYIO_TRY_LOG_ERR(var, expr, fmt, ...) \
    TINYIO_TRY_LOG_ERR_IMPL(var, __COUNTER__, expr, fmt, __VA_ARGS__)

#define TINYIO_TRY_LOG_ERR_IMPL(var, id, expr, fmt, ...)                       \
    auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (expr);                       \
    if (!TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]] {       \
        const auto& _tinyio_err = TINYIO_DETAIL_CAT(_tinyio_try_, id).error(); \
        ELOGFMT(ERROR, "{} failed: code={} " fmt, _tinyio_err.Context(),       \
                _tinyio_err.Value() __VA_OPT__(, ) __VA_ARGS__);               \
        return std::unexpected(_tinyio_err);                                   \
    }                                                                          \
    auto var = ::tinyio::detail::UnwrapExpected(                               \
        std::forward<decltype(TINYIO_DETAIL_CAT(_tinyio_try_, id))>(           \
            TINYIO_DETAIL_CAT(_tinyio_try_, id)))

/// \brief Like \c TINYIO_TRY_VOID but logs the error before propagating it.
#define TINYIO_TRY_LOG_ERR_VOID(expr, fmt, ...) \
    TINYIO_TRY_LOG_ERR_VOID_IMPL(__COUNTER__, expr, fmt, __VA_ARGS__)

#define TINYIO_TRY_LOG_ERR_VOID_IMPL(id, expr, fmt, ...)                       \
    if (auto&& TINYIO_DETAIL_CAT(_tinyio_try_, id) = (expr);                   \
        !TINYIO_DETAIL_CAT(_tinyio_try_, id).has_value()) [[unlikely]] {       \
        const auto& _tinyio_err = TINYIO_DETAIL_CAT(_tinyio_try_, id).error(); \
        ELOGFMT(ERROR, "{} failed: code={} " fmt, _tinyio_err.Context(),       \
                _tinyio_err.Value() __VA_OPT__(, ) __VA_ARGS__);               \
        return std::unexpected(_tinyio_err);                                   \
    }

#ifndef TINYIO_NO_SHORT_MACROS
#    define TRY(var, ...) TINYIO_TRY(var, __VA_ARGS__)
#    define TRY_VOID(...) TINYIO_TRY_VOID(__VA_ARGS__)
#    define TRY_LOG_ERR(var, expr, fmt, ...) TINYIO_TRY_LOG_ERR(var, expr, fmt, __VA_ARGS__)
#    define TRY_LOG_ERR_VOID(expr, fmt, ...) TINYIO_TRY_LOG_ERR_VOID(expr, fmt, __VA_ARGS__)
#endif
