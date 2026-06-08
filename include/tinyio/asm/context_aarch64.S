.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,%function
tinyio_ctx_switch:
    // Save Frame Pointer to current stack
    str x29, [sp, #-16]!

    mov x2, sp
    str x2, [x0, #0]
    str x30, [x0, #8]

    ldr x2, [x1, #0]
    mov sp, x2
    ldr x30, [x1, #8]

    // Restore Frame Pointer from new stack
    ldr x29, [sp], #16
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
