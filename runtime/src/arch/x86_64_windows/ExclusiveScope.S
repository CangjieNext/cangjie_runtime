// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ------------------------------------------------------------------------------
// CJ_MCC_ExclusiveScope — Windows x64
//
// Glue owns pin + protectAddr via CJ_MCC_ExclusiveScopeEnter/Leave; stub only
// when destSp != 0. Switches SP to destSp, builds OS bridge, invokes AutoEnv.
//
// ABI: void CJ_MCC_ExclusiveScope(void* sret, void* fn, uintptr_t destSp)
//      Win64: rcx=sret, rdx=fn, r8=destSp
//
// GC: sret must not live in callee-saved regs across AutoEnv
// (a safepoint may snapshot it as a GC root). Keep sret/fn in the arg regs
// (volatile) through bridge setup; hold stub FA in rax.
// ------------------------------------------------------------------------------

// CJ exclusive stub (Win64 callee-saved: r15,r14,r13,r12,rdi,rsi,rbx):
//   [rbp]     saved rbp | return addr
//   -8..-56   r15,r14,r13,r12,rdi,rsi,rbx   (RecordStubCalleeSaved)
//   -64       saved_cj_rsp          (Exception.cpp EXCLUSIVE_STUB_SAVED_CJ_SP_OFFSET)
#define ExclusiveScopeFrameSize     (8 * 8)

// OS bridge:
//   [rbp]     saved rbp | return addr
//   -8..-24   N2CSlotData
// Callee-saved live on the CJ stub (RecordStubCalleeSaved); bridge records none.
#define ExclusiveBridgeFrameSize    (8 * 4)

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

    // rcx=sret, rdx=fn stay in arg regs; rax = stub FA for bridge setup.
    movq    %rbp, %rax

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Save CJ SP; switch to validated OS destSp (precondition: != 0).
    movq    %rsp, -64(%rbp)
    movq    %r8, %rsp
    subq    $128, %rsp
    andq    $-16, %rsp

    // Manual frame: saved rbp = CJ exclusive stub FA; RA = real return to glue.
    pushq   8(%rax)
    pushq   %rax
    movq    %rsp, %rbp
    subq    $ExclusiveBridgeFrameSize, %rsp

    // Fill N2CSlotData at fa-24 (do not clobber rcx/rdx).
    leaq    unwindPCForExclusiveStub(%rip), %r9
    movq    %r9, -24(%rbp)          // slot->pc
    movq    %rax, -16(%rbp)         // slot->fa
    movq    $2, -8(%rbp)            // slot->status = RISKY

    // AutoEnv Invoke: *(fn+8)(sret, fn, OuterTypeInfo=*(fn+0))
    // Win64: rcx=sret, rdx=this, r8=OuterTypeInfo; need 32-byte shadow.
    // rcx/rdx still hold sret/fn from entry.
    movq    (%rdx), %r8             // OuterTypeInfo
    movq    8(%rdx), %r11           // GENERIC_VIRTUAL_FUNC
    subq    $32, %rsp
    callq   *%r11
    addq    $32, %rsp

    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

    movq    %rbp, %rsp
    popq    %rbp
    addq    $8, %rsp

    movq    -64(%rbp), %rsp

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

    .seh_endproc
