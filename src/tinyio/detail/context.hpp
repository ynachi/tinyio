#pragma once

#include <cstddef>

/// Stackful context switching for fibers: tinyio's replacement for
/// Boost.Context's fcontext. The assembly lives in context.cc.
///
/// A suspended context is nothing but a stack pointer. Everything else the
/// context needs to resume -- the callee-saved registers and the address to
/// return to -- was pushed on its own stack by the switch that suspended it:
///
///   high addresses
///   +--------------------+
///   | the context's own  |
///   | frames             |
///   +--------------------+
///   | return address     |  <- where execution continues on resume
///   | callee-saved regs  |
///   +--------------------+  <- Context::sp
///   low addresses
///
/// Switching is therefore: push my callee-saved registers, store my sp,
/// load the other sp, pop its callee-saved registers, return. Only the
/// callee-saved registers need saving because the switch is an ordinary
/// function call: the compiler already assumes the call clobbers the rest.

namespace tinyio::detail::context {

struct Context {
  /// Saved stack pointer while suspended. Meaningless while running.
  void* sp = nullptr;
};

/// Entry point of a new context. It must never return: there is no frame
/// to return to. A fiber ends by switching away for the last time.
using EntryFn = void (*)(void* arg) noexcept;

/// Prepares @p ctx so that the first Switch() to it calls fn(arg) on the
/// stack whose highest address is @p stack_top (stacks grow down).
void Make(Context& ctx, void* stack_top, EntryFn fn, void* arg) noexcept;

}  // namespace tinyio::detail::context

/// Saves the running context's stack pointer into *save_sp and resumes the
/// context whose stack pointer is load_sp. Returns when something switches
/// back to the saved context.
extern "C" void TinyioSwitchContext(void** save_sp, void* load_sp) noexcept;

namespace tinyio::detail::context {

/// Suspends the running context into @p from and resumes @p to.
inline void Switch(Context& from, const Context& to) noexcept {
  TinyioSwitchContext(&from.sp, to.sp);
}

}  // namespace tinyio::detail::context
