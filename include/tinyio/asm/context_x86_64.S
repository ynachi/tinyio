// x86-64 context switch for tinyio
//
// Both Clang and GCC (≥15) support __attribute__((preserve_none)), which
// passes the first two integer arguments in %r12 and %r13 and treats
// all GPRs except %rsp and %rbp as caller-saved.  The compiler saves
// only the registers that are actually live at each call site.
//
// This means the assembly only needs to save/restore %rbp (the frame
// pointer) plus the two Context fields (rsp, rip).

#define ARG0 %r12
#define ARG1 %r13

// CET (Control-flow Enforcement Technology) support.
// On systems compiled with -fcf-protection, indirect branches must
// land on an ENDBR64 instruction or the CPU raises #CP.
#ifdef __CET__
#include <cet.h>
#define ENDBR64 endbr64
#else
#define ENDBR64
#endif

.text
.globl tinyio_ctx_switch
.type tinyio_ctx_switch,@function
tinyio_ctx_switch:
    ENDBR64

    // Save frame pointer to the current stack
    pushq %rbp

    // Save current SP and resume address into from->context
    movq %rsp, 0(ARG0)
    leaq .Ltinyio_resume(%rip), %rax
    movq %rax, 8(ARG0)

    // Restore target SP and frame pointer from to->context
    movq 0(ARG1), %rsp
    popq %rbp

    // Jump to the target's resume address (or tinyio_fiber_start on first run)
    jmp *8(ARG1)

.Ltinyio_resume:
    ENDBR64
    ret
.size tinyio_ctx_switch, .-tinyio_ctx_switch

.globl tinyio_fiber_start
.type tinyio_fiber_start,@function
tinyio_fiber_start:
    ENDBR64
    popq %rdi
    call tinyio_fiber_entry
    ud2
.size tinyio_fiber_start, .-tinyio_fiber_start

.section .note.GNU-stack,"",@progbits
