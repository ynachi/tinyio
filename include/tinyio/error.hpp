#pragma once
#include <expected>
#include <string_view>
#include <system_error>
#include <format>

namespace tinyio
{
struct Error
{
    int code = 0;
    const char* op = "";

    /// Returns this error as a generic POSIX std::error_code.
    [[nodiscard]] std::error_code error_code() const noexcept { return {code, std::generic_category()}; }

    /// \brief Construct an \c Error from a positive errno value.
    /// \param e POSIX errno (positive).
    /// \param ctx Static context string (must outlive the Error; use a literal).
    [[nodiscard]] static constexpr Error from_errno(const int e, const char* ctx = "") noexcept
    {
        return Error{e, ctx != nullptr ? ctx : ""};
    }

    /// Returns true when this object contains a non-zero errno value.
    [[nodiscard]] explicit operator bool() const noexcept { return code != 0; }

    /// Returns the system error message string for this error.
    [[nodiscard]] std::string message() const { return error_code().message(); }
};

}  // namespace tinyio

template <>
struct std::formatter<tinyio::Error> : std::formatter<std::string>
{
    template <typename FormatContext>
    auto format(const tinyio::Error& err, FormatContext& ctx) const
    {
        return std::formatter<std::string>::format(
            std::format("{} ({})", err.message(), err.code), ctx);
    }
};

namespace tinyio
{

/// \brief Equality compares only the code (context is diagnostic, not identity).
[[nodiscard]] constexpr bool operator==(const Error& a, const Error& b) noexcept
{
    return a.code == b.code;
}

template <class T>
using Result = std::expected<T, Error>;

/// \brief Convenience: build an error-state \c Result payload.
/// \param code Positive errno-style code.
/// \param ctx  Static context string.
[[nodiscard]] inline std::unexpected<Error> fail(const int code, const char* ctx = "") noexcept
{
    return std::unexpected(Error::from_errno(code, ctx));
}
}  // namespace tinyio

namespace tinyio::detail
{

template <class T, class E>
decltype(auto) unwrap_expected(std::expected<T, E>& result) noexcept
{
    if constexpr (!std::is_void_v<T>)
    {
        return T(std::move(*result));
    }
}

}  // namespace tinyio::detail

// ---------------------------------------------------------------------------
// Propagation macros (GNU statement-expression; Linux-first, GCC/Clang only).
// ---------------------------------------------------------------------------

/// \brief Rust `?`-style propagation.
///
/// Evaluates \c expr (a \c Result<T>); on error, returns the error from the
/// enclosing function; on success, yields the unwrapped \c T value.
///
/// \code
///   auto n = TRY(io.read(fd, buf));   // n is size_t on success
///   TRY(io.sleep_for(10ms));          // Result<void>: just propagate
/// \endcode
#define TRY(EXPR) \
  ({                                                                     \
      auto _tinyio_res = (EXPR);                                         \
      if (!_tinyio_res) {                                                \
          return std::unexpected(_tinyio_res.error());                   \
      }                                                                  \
      ::tinyio::detail::unwrap_expected(_tinyio_res);                    \
  })
/// \brief Like \c UX_TRY but logs error before propagating.
/// \param expr A \c Result<T>.
/// \param msg  A \c const \c char* description logged on failure.
#define TRY_LOG_ERR(EXPR, FMT, ...)                                                                 \
    ({                                                                                              \
        auto _tinyio_res = (EXPR);                                                                  \
        if (!_tinyio_res)                                                                           \
        {                                                                                           \
            const auto _tinyio_err = _tinyio_res.error();                                           \
            ALOG_ERROR("{} failed: code={} " FMT, _tinyio_err.op ? _tinyio_err.op : "unknown", _tinyio_err.code, ##__VA_ARGS__); \
            return std::unexpected(_tinyio_err);                                                    \
        }                                                                                           \
        ::tinyio::detail::unwrap_expected(_tinyio_res);                                             \
    })
