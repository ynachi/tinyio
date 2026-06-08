// AArch64 context switch for tinyio
// Standard AAPCS64:
// Callee-saved registers: x19-x30 and d8-d15
// Arguments: x0 (from), x1 (to)
// Return address (LR/x30) is popped from the stack.

.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,%function
tinyio_ctx_switch:
    // BTI landing pad for indirect branches (-mbranch-protection)
    bti c

    // Allocate 160 bytes for 20 64-bit registers
    sub sp, sp, #160

    // Store FPRs
    stp d8, d9, [sp, #0]
    stp d10, d11, [sp, #16]
    stp d12, d13, [sp, #32]
    stp d14, d15, [sp, #48]

    // Store GPRs
    stp x19, x20, [sp, #64]
    stp x21, x22, [sp, #80]
    stp x23, x24, [sp, #96]
    stp x25, x26, [sp, #112]
    stp x27, x28, [sp, #128]
    stp x29, x30, [sp, #144]

    // Save current SP to from->sp
    mov x2, sp
    str x2, [x0]

    // Load target SP from to->sp
    ldr x2, [x1]
    mov sp, x2

    // Load GPRs
    ldp x29, x30, [sp, #144]
    ldp x27, x28, [sp, #128]
    ldp x25, x26, [sp, #112]
    ldp x23, x24, [sp, #96]
    ldp x21, x22, [sp, #80]
    ldp x19, x20, [sp, #64]

    // Load FPRs
    ldp d14, d15, [sp, #48]
    ldp d12, d13, [sp, #32]
    ldp d10, d11, [sp, #16]
    ldp d8, d9, [sp, #0]

    // Deallocate 160 bytes
    add sp, sp, #160

    ret
.size tinyio_ctx_switch, .-tinyio_ctx_switch

.globl tinyio_fiber_start
.type tinyio_fiber_start,%function
tinyio_fiber_start:
    bti c
    // Setup a dummy stack frame to appease sanitizers
    mov x29, #0
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    // x19 contains the fiber pointer as set in IO::spawn
    mov x0, x19
    bl tinyio_fiber_entry
    brk #0
.size tinyio_fiber_start, .-tinyio_fiber_start

.section .note.GNU-stack,"",%progbits
