.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,@function
tinyio_ctx_switch:
    pushq %rbp
    movq %rsp, 0(%rdi)
    leaq .Ltinyio_resume(%rip), %rax
    movq %rax, 8(%rdi)

    movq 0(%rsi), %rsp
    popq %rbp
    jmp *8(%rsi)

.Ltinyio_resume:
    ret
.size tinyio_ctx_switch, .-tinyio_ctx_switch

.globl tinyio_fiber_start
.type tinyio_fiber_start,@function
tinyio_fiber_start:
    popq %rdi
    call tinyio_fiber_entry
    ud2
.size tinyio_fiber_start, .-tinyio_fiber_start

.section .note.GNU-stack,"",@progbits
