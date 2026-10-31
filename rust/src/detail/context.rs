//! The context switch: a port of the C++ `detail/context.cc`, instruction for
//! instruction (Rule 10: do not edit the assembly after stage 1).
//!
//! A suspended context is nothing but a stack pointer. Everything else it
//! needs to resume (callee-saved registers, FP control state, the address to
//! return to) was pushed on its own stack by the switch that suspended it:
//!
//! ```text
//!   high addresses
//!   +--------------------+
//!   | the context's own  |
//!   | frames             |
//!   +--------------------+
//!   | return address     |  <- where execution continues on resume
//!   | callee-saved regs  |
//!   +--------------------+  <- Context::sp
//!   low addresses
//! ```
//!
//! `tinyio_switch_context(save_sp, load_sp)`:
//!   1. push the callee-saved registers (and MXCSR / x87 CW, or FPCR) on the
//!      current stack,
//!   2. `*save_sp = sp`,
//!   3. `sp = load_sp`,
//!   4. pop the callee-saved registers of the context being resumed,
//!   5. `ret`, to wherever THAT context called the switch from or, the first
//!      time, to `tinyio_context_trampoline`.
//!
//! The trampoline moves the entry argument into the first-argument register
//! and calls the entry function, which never returns (a fiber ends by
//! switching away for good); the trampoline traps if it does. Its CFI marks it
//! as the outermost frame, so unwinders and debuggers do not walk off the top
//! of the fiber's stack.
//!
//! aarch64: pointer authentication and branch protection would sign the
//! return address in the caller's prologue; this switch round-trips x30
//! through memory unsigned and has no `bti` landing pads. Plan open decision 7:
//! do nothing unless an arm build ever fails, then add a `build.rs` check.

use std::arch::global_asm;
use std::cell::Cell;
use std::ffi::c_void;

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
compile_error!("tinyio: context switching is implemented for x86_64 and aarch64 only");

// System V AMD64. Callee-saved: rbx, rbp, r12-r15, plus the control bits of
// MXCSR (SSE rounding / exception masks) and the x87 control word.
// Arguments: rdi = save_sp, rsi = load_sp.
#[cfg(target_arch = "x86_64")]
global_asm!(
    r#"
  .pushsection .text
  .globl  tinyio_switch_context
  .hidden tinyio_switch_context
  .type   tinyio_switch_context, @function
  .p2align 4
tinyio_switch_context:
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
  .size   tinyio_switch_context, .-tinyio_switch_context

  .globl  tinyio_context_trampoline
  .hidden tinyio_context_trampoline
  .type   tinyio_context_trampoline, @function
  .p2align 4
tinyio_context_trampoline:
  .cfi_startproc
  .cfi_undefined %rip
  movq    %r12, %rdi
  callq   *%r13
  ud2
  .cfi_endproc
  .size   tinyio_context_trampoline, .-tinyio_context_trampoline
  .popsection
"#,
    options(att_syntax)
);

// AAPCS64. Callee-saved: x19-x28, the frame pointer x29, the link register
// x30 (the return address), the low 64 bits of v8-v15 (d8-d15), and FPCR.
// Arguments: x0 = save_sp, x1 = load_sp. sp must stay 16-byte aligned; the
// 176-byte frame keeps it so (160 bytes of registers, 8 for FPCR, 8 of
// alignment padding).
#[cfg(target_arch = "aarch64")]
global_asm!(
    r#"
  .pushsection .text
  .globl  tinyio_switch_context
  .hidden tinyio_switch_context
  .type   tinyio_switch_context, %function
  .p2align 4
tinyio_switch_context:
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
  .size   tinyio_switch_context, .-tinyio_switch_context

  .globl  tinyio_context_trampoline
  .hidden tinyio_context_trampoline
  .type   tinyio_context_trampoline, %function
  .p2align 4
tinyio_context_trampoline:
  .cfi_startproc
  .cfi_undefined x30
  mov     x0, x19
  blr     x20
  brk     #0
  .cfi_endproc
  .size   tinyio_context_trampoline, .-tinyio_context_trampoline
  .popsection
"#
);

unsafe extern "C" {
    fn tinyio_switch_context(save_sp: *mut *mut u8, load_sp: *mut u8);
    fn tinyio_context_trampoline();
}

/// The entry point of a new context. It must never return: there is no frame
/// to return to. A fiber ends by switching away for the last time.
pub(crate) type EntryFn = extern "C" fn(*mut c_void) -> !;

/// A suspended execution context: its saved stack pointer.
pub(crate) struct Context {
    sp: Cell<*mut u8>,
}

/// The fake frame a new context starts from. Field order = ascending
/// addresses = the order `tinyio_switch_context` pops them in.
#[cfg(target_arch = "x86_64")]
#[repr(C)]
struct InitialFrame {
    mxcsr: u32,
    x87_cw: u16,
    padding: u16,
    r15: usize,
    r14: usize,
    r13: usize, // entry function
    r12: usize, // its argument
    rbx: usize,
    rbp: usize,            // 0: ends frame-pointer backtraces
    return_address: usize, // tinyio_context_trampoline
}
#[cfg(target_arch = "x86_64")]
const _: () = assert!(size_of::<InitialFrame>() == 64);

#[cfg(target_arch = "aarch64")]
#[repr(C)]
struct InitialFrame {
    x19: usize, // the entry function's argument
    x20: usize, // entry function
    x21_x28: [usize; 8],
    x29: usize, // 0: ends frame-pointer backtraces
    x30: usize, // return address: tinyio_context_trampoline
    d8_d15: [f64; 8],
    fpcr: usize,
    padding: usize, // keeps the frame 16-byte aligned (176 total)
}
#[cfg(target_arch = "aarch64")]
const _: () = assert!(size_of::<InitialFrame>() == 176);

impl Context {
    pub(crate) const fn empty() -> Context {
        Context {
            sp: Cell::new(std::ptr::null_mut()),
        }
    }

    /// Prepare `self` so that the first [`switch`] to it calls `entry(arg)` on
    /// the stack whose highest usable address is `stack_top` (stacks grow down).
    ///
    /// # Safety
    /// `stack_top` must be the end of a writable region at least
    /// `size_of::<InitialFrame>() + 15` bytes long that nothing else uses,
    /// and the region must stay mapped until the context is finished.
    pub(crate) unsafe fn make(&self, stack_top: *mut u8, entry: EntryFn, arg: *mut c_void) {
        // The frame sits at the very top of the stack, 16-byte aligned. When
        // the switch has popped it, sp == top: aligned, as the ABI requires at
        // the trampoline's call into `entry`.
        let top = stack_top.map_addr(|a| a & !15);
        // SAFETY: caller guarantees the region below `top` is writable and big
        // enough for one frame (fn contract above).
        let frame = unsafe { top.cast::<InitialFrame>().sub(1) };

        let entry = entry as usize;
        let trampoline = tinyio_context_trampoline as unsafe extern "C" fn() as usize;
        let argument = arg as usize;

        #[cfg(target_arch = "x86_64")]
        {
            // Inherit the creating thread's floating-point control state, as
            // a new thread would.
            let mut mxcsr: u32 = 0;
            let mut cw: u16 = 0;
            // SAFETY: both pointers are to live locals of the right width;
            // stmxcsr/fnstcw only store, and touch no other state.
            unsafe {
                std::arch::asm!(
                    "stmxcsr dword ptr [{m}]",
                    "fnstcw word ptr [{c}]",
                    m = in(reg) &raw mut mxcsr,
                    c = in(reg) &raw mut cw,
                    options(nostack, preserves_flags)
                );
            }
            // SAFETY: `frame` is writable per the contract above.
            unsafe {
                frame.write(InitialFrame {
                    mxcsr,
                    x87_cw: cw,
                    padding: 0,
                    r15: 0,
                    r14: 0,
                    r13: entry,
                    r12: argument,
                    rbx: 0,
                    rbp: 0,
                    return_address: trampoline,
                });
            }
        }
        #[cfg(target_arch = "aarch64")]
        {
            let fpcr: usize;
            // SAFETY: reading FPCR has no side effects.
            unsafe {
                std::arch::asm!("mrs {}, fpcr", out(reg) fpcr, options(nomem, nostack, preserves_flags));
            }
            // SAFETY: `frame` is writable per the contract above.
            unsafe {
                frame.write(InitialFrame {
                    x19: argument,
                    x20: entry,
                    x21_x28: [0; 8],
                    x29: 0,
                    x30: trampoline,
                    d8_d15: [0.0; 8],
                    fpcr,
                    padding: 0,
                });
            }
        }
        self.sp.set(frame.cast::<u8>());
    }
}

/// Suspend the running context into `from` and resume `to`. Returns when
/// something switches back to `from`.
///
/// # Safety
/// `to` must be a context prepared by [`Context::make`] and not yet finished,
/// or one saved by an earlier `switch`, and its stack must still be mapped. The
/// caller must be running on the stack `from` stands for, and must hold no
/// `&mut` to anything the resumed context may touch (Rule 5).
pub(crate) unsafe fn switch(from: &Context, to: &Context) {
    // SAFETY: `from.sp.as_ptr()` is a valid, writable slot; `to.sp` holds a
    // stack pointer whose frame is laid out as the assembly pops it (a `make`
    // frame or one this same routine pushed). Everything else is the fn contract.
    unsafe { tinyio_switch_context(from.sp.as_ptr(), to.sp.get()) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn initial_frame_is_the_documented_size() {
        #[cfg(target_arch = "x86_64")]
        assert_eq!(size_of::<InitialFrame>(), 64);
        #[cfg(target_arch = "aarch64")]
        assert_eq!(size_of::<InitialFrame>(), 176);
    }

    #[test]
    fn empty_context_is_null() {
        assert!(Context::empty().sp.get().is_null());
    }
}
