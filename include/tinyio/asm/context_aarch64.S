#if defined(__clang__)
#define ARG0 x20
#define ARG1 x21
#else
#define ARG0 x0
#define ARG1 x1
#endif

.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,%function
tinyio_ctx_switch:
#if !defined(__clang__)
    stp d14, d15, [sp, #-16]!
    stp d12, d13, [sp, #-16]!
    stp d10, d11, [sp, #-16]!
    stp d8,  d9,  [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
#endif

    // Save Frame Pointer to current stack
    str x29, [sp, #-16]!

    mov x2, sp
    str x2, [ARG0, #0]
    str x30, [ARG0, #8]

    ldr x2, [ARG1, #0]
    mov sp, x2
    ldr x30, [ARG1, #8]

    // Restore Frame Pointer from new stack
    ldr x29, [sp], #16

#if !defined(__clang__)
    ldp x19, x20, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x27, x28, [sp], #16
    ldp d8,  d9,  [sp], #16
    ldp d10, d11, [sp], #16
    ldp d12, d13, [sp], #16
    ldp d14, d15, [sp], #16
#endif

    ret
.size tinyio_ctx_switch, .-tinyio_ctx_switch

.globl tinyio_fiber_start
.type tinyio_fiber_start,%function
tinyio_fiber_start:
    ldr x0, [sp], #16
    bl tinyio_fiber_entry
    brk #0
.size tinyio_fiber_start, .-tinyio_fiber_start

.section .note.GNU-stack,"",%progbits
