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
// Single-CJThread ExclusiveScope (redesign) — Windows x64
//
// Same control flow as linux x86_64 ExclusiveScope.S, but Win64 ABI/SEH:
//   - Args in rcx/rdx (not rdi/rsi); managed invoke uses rcx=sret, rdx/r8=closure.
//   - Callee-saved include rdi/rsi (RecordStubCalleeSaved / Bridge on _WIN64).
//   - SEH unwind info (.seh_*) instead of DWARF .cfi_*.
//   - Frame allocation provides the 32-byte shadow home space for callees.
//
// Two frames: CJ exclusive stub (heap / current) + exclusive bridge (OS / current)
// with N2CSlotData jumping GC/EH from bridge → CJ stub. No saferegion: one
// mutator stops at safepoint polls. Nested cnt>1 invokes under outer bridge only.
// destSp == 0: no switch/bridge; destSp != 0: switch and build bridge.
// ------------------------------------------------------------------------------

// CJ exclusive stub (Win64 callee-saved: r15,r14,r13,r12,rdi,rsi,rbx):
//   [rbp]     saved rbp | return addr
//   -8..-56   r15,r14,r13,r12,rdi,rsi,rbx   (RecordStubCalleeSaved)
//   -64       executeClosure
//   -72       closurePtr
//   -80       saved_cj_rsp (0 if no stack switch; non-zero = SP to restore)
//   -88       thread / scratch
//   -112/-128 xmm spill / sret scratch
#define ExclusiveScopeFrameSize     (8 * 20)

// OS bridge:
//   [rbp]     saved rbp | return addr
//   -8..-24   N2CSlotData (GetSlotData = fa-24)  // pc/fa/status → CJ stub
//   -32..-80  r15,r14,r13,r12,rdi,rsi,rbx   (RecordExclusiveBridgeCalleeSaved)
//   -88       executeClosure
//   -96       closurePtr
//   -104      exclusive stub fa
//   -128/-144 xmm0/xmm1 spill
#define ExclusiveBridgeFrameSize    (8 * 22)


// ==============================================================================
// CJ_MCC_ExclusiveScope: pin OS thread, switch to OS stack, run closure, return
// ==============================================================================
//
//   void* CJ_MCC_ExclusiveScope(void* executeClosure, void* closurePtr)
//                                    rcx              rdx

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

    // Order must match RecordStubCalleeSaved for _WIN64 (includes rdi/rsi).
    movq    %r15, -8(%rbp)
    movq    %r14, -16(%rbp)
    movq    %r13, -24(%rbp)
    movq    %r12, -32(%rbp)
    movq    %rdi, -40(%rbp)
    movq    %rsi, -48(%rbp)
    movq    %rbx, -56(%rbp)

    movq    %rcx, -64(%rbp)     // executeClosure
    movq    %rdx, -72(%rbp)     // closurePtr
    movq    $0, -80(%rbp)       // saved_cj_rsp: 0 => no switch

    // ExclusiveScopeState at -152(%rbp): { cnt*, destSp }
    leaq    -152(%rbp), %rcx
    callq   CJ_MCC_ExclusiveScopeEnter

    // destSp == 0: no CJ→OS switch and no bridge — invoke on the current stack.
    movq    -144(%rbp), %r11
    testq   %r11, %r11
    jz      .L_exc_noswitch

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    movq    %rsp, -80(%rbp)
    movq    %r11, %rsp
    subq    $128, %rsp
    andq    $-16, %rsp

.L_exc_build_bridge:
    // Manual frame: saved rbp = CJ exclusive stub FA (cross-stack FP link).
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
    movq    %rdi, -64(%rbp)
    movq    %rsi, -72(%rbp)
    movq    %rbx, -80(%rbp)

    movq    0(%rbp), %r11           // exclusive stub fa
    movq    %r11, -104(%rbp)

    // Args live on the CJ stub frame (may be a different stack than %rbp).
    movq    -64(%r11), %rax
    movq    %rax, -88(%rbp)         // executeClosure
    movq    -72(%r11), %rax
    movq    %rax, -96(%rbp)         // closurePtr

    // slot = { pc: unwindPCForExclusiveStub, fa: CJ stub, status: RISKY }
    movq    %rbp, %rcx              // bridge fa
    movq    -104(%rbp), %rdx        // exclusive stub fa
    callq   MRT_SaveExclusiveBridgeSlot

    // Managed call: Future.executeClosure Win64 ABI — rcx=sret, rdx/r8=closure
    leaq    -120(%rbp), %rcx            // sret scratch for Unit
    movq    -96(%rbp), %rdx             // closurePtr
    movq    %rdx, %r8
    movq    -88(%rbp), %r11             // executeClosure
    movq    8(%r11), %r11
    callq   *%r11
    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

    // Preserve return values
    movq    %rax, %rbx
    movapd  %xmm0, -128(%rbp)
    movapd  %xmm1, -144(%rbp)

    // Tear down bridge (rbx holds integer return)
    movq    -32(%rbp), %r15
    movq    -40(%rbp), %r14
    movq    -48(%rbp), %r13
    movq    -56(%rbp), %r12
    movq    -64(%rbp), %rdi
    movq    -72(%rbp), %rsi
    movq    %rbx, %rax
    movapd  -128(%rbp), %xmm0
    movapd  -144(%rbp), %xmm1

    movq    %rbp, %rsp
    popq    %rbp                    // restore CJ exclusive stub fa into %rbp
    addq    $8, %rsp                // discard bridge return address

.L_exc_return_from_bridge:
    // %rbp is again the CJ exclusive stub. Restore heap SP only if we switched.
    movq    -80(%rbp), %r11
    testq   %r11, %r11
    jz      .L_exc_after_stack_restore
    movq    %r11, %rsp

.L_exc_after_stack_restore:
    movq    %rax, %r13
    movapd  %xmm0, -96(%rbp)
    movapd  %xmm1, -112(%rbp)

    leaq    -152(%rbp), %rcx
    callq   CJ_MCC_ExclusiveScopeLeave

    movq    %r13, %rax
    movapd  -96(%rbp), %xmm0
    movapd  -112(%rbp), %xmm1
    // Fall through into epilogue (hot path).

.L_exc_cleanup:
    movq    -8(%rbp),  %r15
    movq    -16(%rbp), %r14
    movq    -24(%rbp), %r13
    movq    -32(%rbp), %r12
    movq    -40(%rbp), %rdi
    movq    -48(%rbp), %rsi
    movq    -56(%rbp), %rbx

    addq    $ExclusiveScopeFrameSize, %rsp
    popq    %rbp
    retq

// No stack switch (destSp == 0): invoke without a bridge frame.
.L_exc_noswitch:
    leaq    -120(%rbp), %rcx            // sret scratch
    movq    -72(%rbp), %rdx             // closurePtr
    movq    %rdx, %r8
    movq    -64(%rbp), %r11             // executeClosure
    movq    8(%r11), %r11
    callq   *%r11
    // saved_cj_rsp is still 0; join shared CntSub + epilogue.
    jmp     .L_exc_after_stack_restore

    .seh_endproc
