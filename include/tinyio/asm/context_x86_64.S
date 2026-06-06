.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,@function
tinyio_ctx_switch:
    movq %rsp, 0(%rdi)
    leaq .Ltinyio_resume(%rip), %rax
    movq %rax, 8(%rdi)
    movq %rbx, 16(%rdi)
    movq %rbp, 24(%rdi)
    movq %r12, 32(%rdi)
    movq %r13, 40(%rdi)
    movq %r14, 48(%rdi)
    movq %r15, 56(%rdi)

    movq 0(%rsi), %rsp
    movq 16(%rsi), %rbx
    movq 24(%rsi), %rbp
    movq 32(%rsi), %r12
    movq 40(%rsi), %r13
    movq 48(%rsi), %r14
    movq 56(%rsi), %r15
    jmp *8(%rsi)

.Ltinyio_resume:
    ret
.size tinyio_ctx_switch, .-tinyio_ctx_switch

.globl tinyio_fiber_start
.type tinyio_fiber_start,@function
tinyio_fiber_start:
    subq $8, %rsp
    movq %r12, %rdi
    call tinyio_fiber_entry
    ud2
.size tinyio_fiber_start, .-tinyio_fiber_start

.section .note.GNU-stack,"",@progbits
