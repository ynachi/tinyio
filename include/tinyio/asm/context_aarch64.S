// AArch64 context switch for tinyio
//
// Both Clang and GCC (≥16) support __attribute__((preserve_none)), which
// passes the first two integer arguments in x20 and x21 and treats
// all GPRs except SP, x29 (FP), and x30 (LR) as caller-saved.
//
// This means the assembly only needs to save/restore x29 (frame pointer)
// plus the two Context fields (sp, pc/lr).

#define ARG0 x20
#define ARG1 x21

.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,%function
tinyio_ctx_switch:
    // BTI landing pad for indirect branches (-mbranch-protection)
    bti c

    // Save frame pointer to the current stack (pre-decrement, 16-byte aligned)
    str x29, [sp, #-16]!

    // Save current SP and LR (return address) into from->context
    mov x2, sp
    str x2, [ARG0, #0]
    str x30, [ARG0, #8]

    // Restore target SP and LR from to->context
    ldr x2, [ARG1, #0]
    mov sp, x2
    ldr x30, [ARG1, #8]

    // Restore frame pointer from the new stack (post-increment)
    ldr x29, [sp], #16

    ret
.size tinyio_ctx_switch, .-tinyio_ctx_switch

.globl tinyio_fiber_start
.type tinyio_fiber_start,%function
tinyio_fiber_start:
    bti c
    ldr x0, [sp], #16
    bl tinyio_fiber_entry
    brk #0
.size tinyio_fiber_start, .-tinyio_fiber_start

.section .note.GNU-stack,"",%progbits
