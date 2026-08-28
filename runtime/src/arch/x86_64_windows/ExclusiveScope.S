// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define CJTHREAD_THREAD_OFFSET      0x10
#define CJTHREAD_CONTEXT_OFFSET     0x18

#define CONTEXT_RSP                 0x00
#define CONTEXT_RBP                 0x08
#define CONTEXT_RBX                 0x10
#define CONTEXT_RIP                 0x18
#define CONTEXT_R12                 0x20
#define CONTEXT_R13                 0x28
#define CONTEXT_R14                 0x30
#define CONTEXT_R15                 0x38
#define CONTEXT_MXCSR               0x40
#define CONTEXT_FPU_CW              0x44

#define THREAD_PROCESSOR_OFFSET     0x38
#define ExclusiveScopeFrameSize     (8 * 22)

    .text
    .p2align 4, 0x90
    .def CJ_MCC_ExclusiveScope
    .scl 2
    .type 32
    .endef
    .global CJ_MCC_ExclusiveScope
CJ_MCC_ExclusiveScope:
    .seh_proc CJ_MCC_ExclusiveScope
    pushq   %rbp
    .seh_pushreg %rbp
    movq    %rsp, %rbp
    .seh_setframe %rbp, 0
    subq    $ExclusiveScopeFrameSize, %rsp
    .seh_stackalloc ExclusiveScopeFrameSize
    .seh_endprologue

    movq    %r15, -8(%rbp)
    movq    %r14, -16(%rbp)
    movq    %r13, -24(%rbp)
    movq    %r12, -32(%rbp)
    movq    %rbx, -40(%rbp)
    movq    %rcx, -48(%rbp)
    movq    %rdx, -56(%rbp)
    movq    %r15, -64(%rbp)

    callq   CJ_CJThreadGetHandle
    testq   %rax, %rax
    jz      .L_exc_no_cjthread
    movq    %rax, -80(%rbp)
    movq    CJTHREAD_THREAD_OFFSET(%rax), %r12
    testq   %r12, %r12
    jz      .L_exc_no_thread
    movq    %r12, -88(%rbp)
    movq    THREAD_PROCESSOR_OFFSET(%r12), %r11
    movq    %r11, -168(%rbp)

    movq    -48(%rbp), %rcx
    movq    -56(%rbp), %rdx
    movq    $0, %r8
    callq   MCC_NewExclusiveCJThread
    testq   %rax, %rax
    jz      .L_exc_create_failed
    movq    %rax, -160(%rbp)

    movq    %rbp, %rdx
    call    .L_exc_get_pc_win
.L_exc_get_pc_win:
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:
    pop     %rcx
    movq    %r15, %r8
    callq   MRT_SaveC2NContext
    movq    $0, %rcx
    callq   MRT_EnterSaferegion
    movq    %rax, -96(%rbp)
    movq    %rsp, -104(%rbp)

    movq    -80(%rbp), %rax
    leaq    CJTHREAD_CONTEXT_OFFSET(%rax), %r14
    leaq    8(%rsp), %r11
    movq    %r11, CONTEXT_RSP(%r14)
    movq    %rbp, CONTEXT_RBP(%r14)
    movq    %rbx, CONTEXT_RBX(%r14)
    movq    (%rsp), %r11
    movq    %r11, CONTEXT_RIP(%r14)
    movq    %r12, CONTEXT_R12(%r14)
    movq    %r13, CONTEXT_R13(%r14)
    movq    -16(%rbp), %r11
    movq    %r11, CONTEXT_R14(%r14)
    movq    -8(%rbp), %r11
    movq    %r11, CONTEXT_R15(%r14)
    stmxcsr CONTEXT_MXCSR(%r14)
    fnstcw  CONTEXT_FPU_CW(%r14)

    movq    -80(%rbp), %rcx
    callq   IsExclusiveCJThread
    testl   %eax, %eax
    jnz     .L_exc_no_os_stack
    movq    -88(%rbp), %rcx
    callq   ExclusiveGetThreadContext
    testq   %rax, %rax
    jz      .L_exc_no_os_stack
    movq    %rax, %r15
    movq    CONTEXT_RSP(%r15), %rax
    testq   %rax, %rax
    jz      .L_exc_no_os_stack

    movq    %rax, %rsp
    ldmxcsr CONTEXT_MXCSR(%r15)
    fldcw   CONTEXT_FPU_CW(%r15)
    subq    $128, %rsp
    andq    $-16, %rsp
    subq    $32, %rsp
    movq    -88(%rbp), %rcx
    movq    -160(%rbp), %rdx
    callq   ExclusiveExecutor
    movq    %rax, %r13
    movapd  %xmm0, -128(%rbp)
    movapd  %xmm1, -144(%rbp)
    movq    -104(%rbp), %rsp
    movq    -80(%rbp), %rcx
    movq    -88(%rbp), %rdx
    movq    -160(%rbp), %r8
    movq    -168(%rbp), %r9
    callq   ExclusiveRestore
    movq    %r13, -112(%rbp)
    jmp     .L_exc_continue

.L_exc_no_os_stack:
    movq    -88(%rbp), %rcx
    movq    -160(%rbp), %rdx
    subq    $32, %rsp
    callq   ExclusiveExecutor
    addq    $32, %rsp
    movq    %rax, -112(%rbp)
    movapd  %xmm0, -128(%rbp)
    movapd  %xmm1, -144(%rbp)
    movq    -80(%rbp), %rcx
    movq    -88(%rbp), %rdx
    movq    -160(%rbp), %r8
    movq    -168(%rbp), %r9
    subq    $32, %rsp
    callq   ExclusiveRestore
    addq    $32, %rsp

.L_exc_continue:
    movq    -96(%rbp), %rax
    cmpq    $0, %rax
    je      .L_exc_skip_leave
    callq   MRT_LeaveSaferegion
    callq   MRT_GetThreadLocalData
    movq    %rax, %r13
    jmp     .L_exc_do_delete
.L_exc_skip_leave:
    movq    -64(%rbp), %r13
.L_exc_do_delete:
    movq    %r13, %rcx
    callq   MRT_DeleteC2NContext
    movq    -112(%rbp), %rax
    movapd  -128(%rbp), %xmm0
    movapd  -144(%rbp), %xmm1
    jmp     .L_exc_cleanup

.L_exc_create_failed:
.L_exc_no_cjthread:
.L_exc_no_thread:
    xorq    %rax, %rax

.L_exc_cleanup:
    movq    -8(%rbp), %r15
    movq    -16(%rbp), %r14
    movq    -24(%rbp), %r13
    movq    -32(%rbp), %r12
    movq    -40(%rbp), %rbx
    movq    %rbp, %rsp
    popq    %rbp
    ret
    .seh_endproc

    .global ExecuteExclusiveCangjieStub
ExecuteExclusiveCangjieStub:
    .seh_proc ExecuteExclusiveCangjieStub
    pushq   %rbp
    .seh_pushreg %rbp
    movq    %rsp, %rbp
    .seh_setframe %rbp, 0
    subq    $128, %rsp
    .seh_stackalloc 128
    .seh_endprologue
    movq    %r15, -8(%rbp)
    movq    %r14, -16(%rbp)
    movq    %r13, -24(%rbp)
    movq    %r12, -32(%rbp)
    movq    %rbx, -40(%rbp)
    movq    %rcx, -48(%rbp)
    movq    %r8, -56(%rbp)
    movq    %r9, -64(%rbp)
    movq    48(%rbp), %rax
    movq    %rax, -80(%rbp)
    movq    %rax, %r15
    movq    %rbp, %rcx
    callq   MRT_SaveTopManagedContextToN2CStub
    movq    -64(%rbp), %r11
    movq    8(%r11), %r11
    movq    -48(%rbp), %rcx
    movq    -56(%rbp), %rdx
    callq   *%r11

    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:
    movq    %rbp, %rcx
    callq   MRT_RestoreTopManagedContextFromN2CStub
    movq    -8(%rbp), %r15
    movq    -16(%rbp), %r14
    movq    -24(%rbp), %r13
    movq    -32(%rbp), %r12
    movq    -40(%rbp), %rbx
    movq    %rbp, %rsp
    popq    %rbp
    ret
    .seh_endproc
