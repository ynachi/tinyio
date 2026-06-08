#if defined(__clang__)
#define ARG0 %r12
#define ARG1 %r13
#else
#define ARG0 %rdi
#define ARG1 %rsi
#endif

.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,@function
tinyio_ctx_switch:
#if !defined(__clang__)
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
#endif

    pushq %rbp
    movq %rsp, 0(ARG0)
    leaq .Ltinyio_resume(%rip), %rax
    movq %rax, 8(ARG0)

    movq 0(ARG1), %rsp
    popq %rbp

#if !defined(__clang__)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
#endif

    jmp *8(ARG1)

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
