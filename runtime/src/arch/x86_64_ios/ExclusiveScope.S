// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define CONTEXT_RSP                 0x00
#define CONTEXT_MXCSR               0x40
#define CONTEXT_FPU_CW              0x44

#define CJTHREAD_THREAD_OFFSET      0x10

// CJ exclusive stub frame (heap stack, or current stack when switch is skipped):
//   [rbp]     saved rbp | return addr
//   -8..-40   r15,r14,r13,r12,rbx   (RecordStubCalleeSaved)
//   -48       executeClosure
//   -56       closurePtr
//   -64       saved_cj_rsp (0 if no stack switch)
//   -72       thread / scratch
#define ExclusiveScopeFrameSize     (8 * 16)

// OS bridge frame:
//   [rbp]     saved rbp | return addr
//   -8..-24   N2CSlotData (GetSlotData = fa-24)
//   -32..-64  r15,r14,r13,r12,rbx   (RecordExclusiveBridgeCalleeSaved)
//   -72       executeClosure
//   -80       closurePtr
//   -88       exclusive stub fa
//   -112/-128 xmm0/xmm1 spill
#define ExclusiveBridgeFrameSize    (8 * 18)


// ==============================================================================
// _CJ_MCC_ExclusiveScope: pin OS thread, switch to OS stack, run closure, return
// ==============================================================================
//
//   void* _CJ_MCC_ExclusiveScope(void* executeClosure, void* closurePtr)
//                                    rdi              rsi

    .text
    .align 2
    .global _CJ_MCC_ExclusiveScope
    .type _CJ_MCC_ExclusiveScope, @function
_CJ_MCC_ExclusiveScope:
    .cfi_startproc

    pushq   %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq    %rsp, %rbp
    .cfi_def_cfa_register %rbp
    subq    $ExclusiveScopeFrameSize, %rsp

    movq    %r15, -8(%rbp)
    movq    %r14, -16(%rbp)
    movq    %r13, -24(%rbp)
    movq    %r12, -32(%rbp)
    movq    %rbx, -40(%rbp)
    .cfi_rel_offset %rbx, -40
    .cfi_rel_offset %r12, -32
    .cfi_rel_offset %r13, -24
    .cfi_rel_offset %r14, -16
    .cfi_rel_offset %r15, -8

    movq    %rdi, -48(%rbp)     // executeClosure
    movq    %rsi, -56(%rbp)     // closurePtr
    movq    $0, -64(%rbp)       // saved_cj_rsp: 0 => no switch

    // ExclusiveScopeState at -120(%rbp): { cnt*, destSp }. Enter pins,
    // samples destSp, and clears protectAddr when a switch is required.
    leaq    -120(%rbp), %rdi
    callq   _CJ_MCC_ExclusiveScopeEnter

    // destSp == 0: no CJ→OS switch and no bridge — invoke on the current stack.
    movq    -112(%rbp), %r11
    testq   %r11, %r11
    jz      .L_exc_noswitch

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global _unwindPCForExclusiveStub
_unwindPCForExclusiveStub:

    movq    %rsp, -64(%rbp)
    movq    %r11, %rsp
    subq    $128, %rsp
    andq    $-16, %rsp

.L_exc_build_bridge:
    // -------- exclusive bridge frame (switched OS stack or current stack) --------
    movq    %rbp, %r13              // CJ exclusive stub fa
    leaq    .L_exc_return_from_bridge(%rip), %rax
    pushq   %rax                    // return address
    pushq   %r13                    // saved rbp = CJ exclusive stub fa
    movq    %rsp, %rbp
    subq    $ExclusiveBridgeFrameSize, %rsp

    movq    %r15, -32(%rbp)
    movq    %r14, -40(%rbp)
    movq    %r13, -48(%rbp)
    movq    %r12, -56(%rbp)
    movq    %rbx, -64(%rbp)

    movq    0(%rbp), %r11           // exclusive stub fa
    movq    %r11, -88(%rbp)

    movq    -48(%r11), %rax
    movq    %rax, -72(%rbp)         // executeClosure
    movq    -56(%r11), %rax
    movq    %rax, -80(%rbp)         // closurePtr

    // Fill bridge slot → jump to CJ exclusive stub on unwind
    movq    %rbp, %rdi              // bridge fa
    movq    -88(%rbp), %rsi         // exclusive stub fa
    callq   _MRT_SaveExclusiveBridgeSlot

    // Managed call: Future.executeClosure ABI — rdi=sret, rsi/rdx=closure
    leaq    -104(%rbp), %rdi            // sret scratch for Unit
    movq    -80(%rbp), %rsi             // closurePtr
    movq    %rsi, %rdx
    movq    -72(%rbp), %r11             // executeClosure
    movq    8(%r11), %r11
    callq   *%r11
    .global _unwindPCForExclusiveStubFull
_unwindPCForExclusiveStubFull:

    // Preserve return values
    movq    %rax, %rbx
    movapd  %xmm0, -112(%rbp)
    movapd  %xmm1, -128(%rbp)

    // Tear down bridge (rbx holds integer return)
    movq    -32(%rbp), %r15
    movq    -40(%rbp), %r14
    movq    -48(%rbp), %r13
    movq    -56(%rbp), %r12
    movq    %rbx, %rax
    movapd  -112(%rbp), %xmm0
    movapd  -128(%rbp), %xmm1

    movq    %rbp, %rsp
    popq    %rbp                    // restore CJ exclusive stub fa
    addq    $8, %rsp                // discard bridge return address

.L_exc_return_from_bridge:
    // Switch back to CJ heap stack only if we actually switched
    movq    -64(%rbp), %r11
    testq   %r11, %r11
    jz      .L_exc_after_stack_restore
    movq    %r11, %rsp

.L_exc_after_stack_restore:
    movq    %rax, %r13
    movapd  %xmm0, -80(%rbp)
    movapd  %xmm1, -96(%rbp)

    leaq    -120(%rbp), %rdi
    callq   _CJ_MCC_ExclusiveScopeLeave

    movq    %r13, %rax
    movapd  -80(%rbp), %xmm0
    movapd  -96(%rbp), %xmm1
    // Fall through into epilogue (hot path).

.L_exc_cleanup:
    movq    -8(%rbp),  %r15
    movq    -16(%rbp), %r14
    movq    -24(%rbp), %r13
    movq    -32(%rbp), %r12
    movq    -40(%rbp), %rbx

    addq    $ExclusiveScopeFrameSize, %rsp
    popq    %rbp
    .cfi_def_cfa %rsp, 8
    retq

// No stack switch (destSp == 0): invoke without a bridge frame.
.L_exc_noswitch:
    leaq    -104(%rbp), %rdi            // sret scratch
    movq    -56(%rbp), %rsi             // closurePtr
    movq    %rsi, %rdx
    movq    -48(%rbp), %r11             // executeClosure
    movq    8(%r11), %r11
    callq   *%r11
    // saved_cj_rsp is still 0; join shared CntSub + epilogue.
    jmp     .L_exc_after_stack_restore

    .cfi_endproc
    .size _CJ_MCC_ExclusiveScope, .-_CJ_MCC_ExclusiveScope
