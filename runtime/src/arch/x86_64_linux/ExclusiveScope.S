// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define CONTEXT_RSP                 0x00
#define CONTEXT_MXCSR               0x40
#define CONTEXT_FPU_CW              0x44

#define CJTHREAD_THREAD_OFFSET      0x10

// ------------------------------------------------------------------------------
// Single-CJThread ExclusiveScope (redesign)
//
// Why two frames:
//   1) CJ exclusive stub  — lives on the CJThread heap stack (or the current
//      stack when the OS switch is skipped). Its FA is the landing point for
//      GC/EH when unwinding *out* of the exclusive body. Callee-saved here are
//      scanned as FrameType::EXSLUSIVE.
//   2) Exclusive OS bridge — lives on the OS thread stack after the switch
//      (or on the current stack for foreign/N2C). Holds N2CSlotData that points
//      back to the CJ stub so UnwindToCallerContext can jump stacks. Identified
//      by unwindPCForExclusiveStubFull (IsExclusiveBridgeFrame).
//
// Stack after first entry with a real CJ→OS switch:
//   [OS]  managed body … → bridge (slot RISKY→CJ stub) 
//   [CJ]  exclusive stub → callers of exclusiveScope
//
// destSp == 0 (nested exclusive or already off the CJ heap): invoke on the
// current stack with no bridge. destSp != 0: switch to OS stack and build the
// exclusive bridge for GC/EH.
//
// No saferegion / no C2N: one mutator stays in managed context; STW stops it
// via safepoint polls, not by parking a second disposable mutator.
// saved_cj_rsp is only for normal return after the bridge — EH cross-stack
// restore rebases with managed FA+16, not this slot.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack, or current stack when switch is skipped):
//   [rbp]     saved rbp | return addr
//   -8..-40   r15,r14,r13,r12,rbx   (RecordStubCalleeSaved)
//   -48       executeClosure
//   -56       closurePtr
//   -64       saved_cj_rsp (0 if no stack switch; non-zero = SP to restore)
//   -112/-120 ExclusiveScopeState { destSp, cnt* } (cnt at -120)
#define ExclusiveScopeFrameSize     (8 * 16)

// OS bridge frame:
//   [rbp]     saved rbp | return addr
//   -8..-24   N2CSlotData (GetSlotData = fa-24)  // pc/fa/status → CJ stub
//   -32..-64  r15,r14,r13,r12,rbx   (RecordExclusiveBridgeCalleeSaved)
//   -72       executeClosure
//   -80       closurePtr
//   -88       exclusive stub fa
//   -112/-128 xmm0/xmm1 spill
#define ExclusiveBridgeFrameSize    (8 * 18)


// ==============================================================================
// CJ_MCC_ExclusiveScope: pin OS thread, switch to OS stack, run closure, return
// ==============================================================================
//
//   void* CJ_MCC_ExclusiveScope(void* executeClosure, void* closurePtr)
//                                    rdi              rsi
//
// executeClosure is Future.executeClosure-shaped: call *(executeClosure+8)
// with sret + closure args (stdlib boxes exceptions inside the closure).

    .text
    .align 2
    .global CJ_MCC_ExclusiveScope
    .type CJ_MCC_ExclusiveScope, @function
CJ_MCC_ExclusiveScope:
    .cfi_startproc

    pushq   %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq    %rsp, %rbp
    .cfi_def_cfa_register %rbp
    subq    $ExclusiveScopeFrameSize, %rsp

    // Callee-saved order must match RecordStubCalleeSaved (GC register roots).
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
    callq   CJ_MCC_ExclusiveScopeEnter@PLT

    // destSp == 0: no CJ→OS switch and no bridge — invoke on the current stack.
    movq    -112(%rbp), %r11
    testq   %r11, %r11
    jz      .L_exc_noswitch

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Remember CJ SP for normal return; switch to validated OS destSp from Enter.
    movq    %rsp, -64(%rbp)
    movq    %r11, %rsp
    subq    $128, %rsp
    andq    $-16, %rsp

.L_exc_build_bridge:
    // Manual frame: saved rbp = CJ exclusive stub FA so the FP chain and the
    // N2C slot both refer to the same CJ-side frame.
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

    // Args live on the CJ stub frame (may be a different stack than %rbp now).
    movq    -48(%r11), %rax
    movq    %rax, -72(%rbp)         // executeClosure
    movq    -56(%r11), %rax
    movq    %rax, -80(%rbp)         // closurePtr

    // slot = { pc: unwindPCForExclusiveStub, fa: CJ stub, status: RISKY }
    movq    %rbp, %rdi              // bridge fa
    movq    -88(%rbp), %rsi         // exclusive stub fa
    callq   MRT_SaveExclusiveBridgeSlot@PLT

    // Managed call: Future.executeClosure ABI — rdi=sret, rsi/rdx=closure
    leaq    -104(%rbp), %rdi            // sret scratch for Unit
    movq    -80(%rbp), %rsi             // closurePtr
    movq    %rsi, %rdx
    movq    -72(%rbp), %r11             // executeClosure
    movq    8(%r11), %r11
    callq   *%r11
    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

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
    popq    %rbp                    // restore CJ exclusive stub fa into %rbp
    addq    $8, %rsp                // discard bridge return address

.L_exc_return_from_bridge:
    // %rbp is again the CJ exclusive stub. Restore heap SP only if we switched.
    movq    -64(%rbp), %r11
    testq   %r11, %r11
    jz      .L_exc_after_stack_restore
    movq    %r11, %rsp

.L_exc_after_stack_restore:
    movq    %rax, %r13
    movapd  %xmm0, -80(%rbp)
    movapd  %xmm1, -96(%rbp)

    leaq    -120(%rbp), %rdi
    callq   CJ_MCC_ExclusiveScopeLeave@PLT

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
    // saved_cj_rsp is still 0; join shared Leave + epilogue.
    jmp     .L_exc_after_stack_restore

    .cfi_endproc
    .size CJ_MCC_ExclusiveScope, .-CJ_MCC_ExclusiveScope
