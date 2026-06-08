// x86-64 context switch for tinyio
//
// Both Clang and GCC (≥15) support __attribute__((preserve_none)), which
// passes the first two integer arguments in %r12 and %r13 and treats
// all GPRs except %rsp and %rbp as caller-saved.  The compiler saves
// only the registers that are actually live at each call site.
//
// Standard System V AMD64 ABI:
// Callee-saved registers: rbp, rbx, r12, r13, r14, r15
// Arguments: %rdi (from), %rsi (to)
// Return address (rip) is implicitly popped by ret.

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
    // Push callee-saved registers
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    // Save current SP to from->sp
    movq %rsp, 0(%rdi)

    // Load target SP from to->sp
    movq 0(%rsi), %rsp

    // Pop callee-saved registers
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp

    ret
.size tinyio_ctx_switch, .-tinyio_ctx_switch

.globl tinyio_fiber_start
.type tinyio_fiber_start,@function
tinyio_fiber_start:
    ENDBR64
    // r12 contains the fiber pointer as set in IO::spawn
    movq %r12, %rdi
    call tinyio_fiber_entry
    ud2
.size tinyio_fiber_start, .-tinyio_fiber_start

.section .note.GNU-stack,"",@progbits
