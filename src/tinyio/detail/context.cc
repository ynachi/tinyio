#include "tinyio/detail/context.hpp"

#include <cstdint>

// The context switch, in assembly. One function per architecture,
// TinyioSwitchContext(void** save_sp, void* load_sp):
//
//   1. push the callee-saved registers on the current stack
//   2. *save_sp = sp
//   3. sp = load_sp
//   4. pop the callee-saved registers of the context being resumed
//   5. return -- to wherever THAT context called TinyioSwitchContext from,
//      or, the first time, to TinyioContextTrampoline (see Make()).
//
// The frame layout pushed in step 1 is mirrored by InitialFrame below: a
// brand-new context is a stack holding a fake frame, as if it had already
// switched away once.
//
// TinyioContextTrampoline is where a new context starts. Make() leaves the
// entry function and its argument in callee-saved registers, which step 4
// restores; the trampoline moves the argument into the first-argument
// register and calls the entry function. The entry function never returns
// (a fiber ends by switching away for good), so the trampoline traps if it
// ever does. Its CFI marks it as the outermost frame, which keeps debugger
// backtraces and unwinders from walking past the top of the fiber's stack.
//
// .hidden: the symbols are internal to libtinyio, not part of its ABI.

// arm64 pointer authentication and branch protection sign the return
// address in the caller's prologue and authenticate it on return. This
// switch round-trips x30 through memory as a plain unsigned value and has
// no bti landing pads, so a build with either enabled would fault at
// runtime. Fail the build instead.
#if defined(__aarch64__) && \
    (defined(__ARM_FEATURE_PAC_DEFAULT) || defined(__ARM_FEATURE_BTI_DEFAULT))
#    error "tinyio's context switch requires -mbranch-protection=none"
#endif

#if defined(__x86_64__)

// System V AMD64. Callee-saved: rbx, rbp, r12-r15, plus the control bits of
// MXCSR (SSE rounding / exception masks) and the x87 control word.
// Arguments: rdi = save_sp, rsi = load_sp.
asm(R"(
  .pushsection .text
  .globl  TinyioSwitchContext
  .hidden TinyioSwitchContext
  .type   TinyioSwitchContext, @function
  .p2align 4
TinyioSwitchContext:
  .cfi_startproc
  pushq   %rbp
  .cfi_adjust_cfa_offset 8
  .cfi_offset %rbp, -16
  pushq   %rbx
  .cfi_adjust_cfa_offset 8
  .cfi_offset %rbx, -24
  pushq   %r12
  .cfi_adjust_cfa_offset 8
  .cfi_offset %r12, -32
  pushq   %r13
  .cfi_adjust_cfa_offset 8
  .cfi_offset %r13, -40
  pushq   %r14
  .cfi_adjust_cfa_offset 8
  .cfi_offset %r14, -48
  pushq   %r15
  .cfi_adjust_cfa_offset 8
  .cfi_offset %r15, -56
  subq    $8, %rsp
  .cfi_adjust_cfa_offset 8
  stmxcsr (%rsp)
  fnstcw  4(%rsp)

  movq    %rsp, (%rdi)
  movq    %rsi, %rsp

  ldmxcsr (%rsp)
  fldcw   4(%rsp)
  addq    $8, %rsp
  .cfi_adjust_cfa_offset -8
  popq    %r15
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r15
  popq    %r14
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r14
  popq    %r13
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r13
  popq    %r12
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r12
  popq    %rbx
  .cfi_adjust_cfa_offset -8
  .cfi_restore %rbx
  popq    %rbp
  .cfi_adjust_cfa_offset -8
  .cfi_restore %rbp
  ret
  .cfi_endproc
  .size   TinyioSwitchContext, .-TinyioSwitchContext

  .globl  TinyioContextTrampoline
  .hidden TinyioContextTrampoline
  .type   TinyioContextTrampoline, @function
  .p2align 4
TinyioContextTrampoline:
  .cfi_startproc
  .cfi_undefined %rip
  movq    %r12, %rdi
  callq   *%r13
  ud2
  .cfi_endproc
  .size   TinyioContextTrampoline, .-TinyioContextTrampoline
  .popsection
)");

#elif defined(__aarch64__)

// AAPCS64. Callee-saved: x19-x28, the frame pointer x29, the link register
// x30 (the return address), the low 64 bits of v8-v15 (d8-d15), and FPCR
// (the FP control register: rounding mode, exception masks -- the arm64
// counterpart of the x86 MXCSR/x87 control word, and per-context the same
// way). Arguments: x0 = save_sp, x1 = load_sp. sp must stay 16-byte
// aligned; the 176-byte frame keeps it so (160 bytes of registers, 8 for
// FPCR, 8 of alignment padding).
asm(R"(
  .pushsection .text
  .globl  TinyioSwitchContext
  .hidden TinyioSwitchContext
  .type   TinyioSwitchContext, %function
  .p2align 4
TinyioSwitchContext:
  .cfi_startproc
  sub     sp, sp, #176
  .cfi_adjust_cfa_offset 176
  stp     x19, x20, [sp, #0]
  stp     x21, x22, [sp, #16]
  stp     x23, x24, [sp, #32]
  stp     x25, x26, [sp, #48]
  stp     x27, x28, [sp, #64]
  stp     x29, x30, [sp, #80]
  .cfi_offset x29, -96
  .cfi_offset x30, -88
  stp     d8,  d9,  [sp, #96]
  stp     d10, d11, [sp, #112]
  stp     d12, d13, [sp, #128]
  stp     d14, d15, [sp, #144]
  mrs     x9, fpcr
  str     x9, [sp, #160]

  mov     x9, sp
  str     x9, [x0]
  mov     sp, x1

  ldr     x9, [sp, #160]
  msr     fpcr, x9
  ldp     x19, x20, [sp, #0]
  ldp     x21, x22, [sp, #16]
  ldp     x23, x24, [sp, #32]
  ldp     x25, x26, [sp, #48]
  ldp     x27, x28, [sp, #64]
  ldp     x29, x30, [sp, #80]
  .cfi_restore x29
  .cfi_restore x30
  ldp     d8,  d9,  [sp, #96]
  ldp     d10, d11, [sp, #112]
  ldp     d12, d13, [sp, #128]
  ldp     d14, d15, [sp, #144]
  add     sp, sp, #176
  .cfi_adjust_cfa_offset -176
  ret
  .cfi_endproc
  .size   TinyioSwitchContext, .-TinyioSwitchContext

  .globl  TinyioContextTrampoline
  .hidden TinyioContextTrampoline
  .type   TinyioContextTrampoline, %function
  .p2align 4
TinyioContextTrampoline:
  .cfi_startproc
  .cfi_undefined x30
  mov     x0, x19
  blr     x20
  brk     #0
  .cfi_endproc
  .size   TinyioContextTrampoline, .-TinyioContextTrampoline
  .popsection
)");

#else
#    error "tinyio: context switching is implemented for x86_64 and aarch64 only"
#endif

extern "C" void TinyioContextTrampoline() noexcept;

namespace tinyio::detail::context {

namespace {

// The fake frame a new context starts from. Field order = ascending
// addresses = the order TinyioSwitchContext pops them in.
#if defined(__x86_64__)
struct InitialFrame {
    std::uint32_t mxcsr;
    std::uint16_t x87_cw;
    std::uint16_t padding;
    std::uintptr_t r15;
    std::uintptr_t r14;
    std::uintptr_t r13;  // entry function
    std::uintptr_t r12;  // its argument
    std::uintptr_t rbx;
    std::uintptr_t rbp;             // 0: ends frame-pointer backtraces
    std::uintptr_t return_address;  // TinyioContextTrampoline
};
static_assert(sizeof(InitialFrame) == 64);
#elif defined(__aarch64__)
struct InitialFrame {
    std::uintptr_t x19;  // the entry function's argument
    std::uintptr_t x20;  // entry function
    std::uintptr_t x21_x28[8];
    std::uintptr_t x29;  // 0: ends frame-pointer backtraces
    std::uintptr_t x30;  // return address: TinyioContextTrampoline
    double d8_d15[8];
    std::uintptr_t fpcr;     // FP control state restored on first entry
    std::uintptr_t padding;  // keeps the frame 16-byte aligned (176 total)
};
static_assert(sizeof(InitialFrame) == 176);
#endif

}  // namespace

void Make(Context& ctx, void* stack_top, const EntryFn fn, void* arg) noexcept {
    // The frame sits at the very top of the stack, 16-byte aligned. When the
    // switch has popped it, sp == top: aligned, as the ABI requires at the
    // trampoline's call into fn.
    auto top = reinterpret_cast<std::uintptr_t>(stack_top) & ~std::uintptr_t{15};
    auto* frame = reinterpret_cast<InitialFrame*>(top) - 1;
    *frame = InitialFrame{};

    const auto entry = reinterpret_cast<std::uintptr_t>(fn);
    const auto trampoline = reinterpret_cast<std::uintptr_t>(&TinyioContextTrampoline);
    const auto argument = reinterpret_cast<std::uintptr_t>(arg);

#if defined(__x86_64__)
    // Inherit the creating thread's floating-point control state, as a new
    // thread would.
    std::uint16_t x87_cw = 0;
    asm volatile("fnstcw %0" : "=m"(x87_cw));
    frame->mxcsr = __builtin_ia32_stmxcsr();
    frame->x87_cw = x87_cw;
    frame->r13 = entry;
    frame->r12 = argument;
    frame->return_address = trampoline;
#elif defined(__aarch64__)
    // Inherit the creating thread's FP control state, as a new thread would.
    std::uint64_t fpcr = 0;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    frame->fpcr = fpcr;
    frame->x19 = argument;
    frame->x20 = entry;
    frame->x30 = trampoline;
#endif

    ctx.sp = frame;
}

}  // namespace tinyio::detail::context
