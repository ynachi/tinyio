#pragma once

#include <boost/context/detail/fcontext.hpp>

namespace tinyio::detail::context {

struct Context {
  boost::context::detail::fcontext_t fctx{nullptr};
};

}  // namespace tinyio::detail::context

extern "C" [[noreturn]] void TinyioFiberEntry(
    boost::context::detail::transfer_t t) noexcept;
