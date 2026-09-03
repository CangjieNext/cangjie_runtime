// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ------------------------------------------------------------------------------
// CJ_MCC_ExclusiveScope
//
// Glue (CodeGen) owns pin + protectAddr via CJ_MCC_ExclusiveScopeEnter/Leave and
// only calls this stub when destSp != 0. This stub:
//   1) builds the CJ exclusive stub frame on the heap stack
//   2) switches SP to destSp
//   3) builds the exclusive OS bridge and invokes the AutoEnv closure
//
// protectAddr and exclusiveScopeCnt are owned by Enter/Leave in the glue finally
// (EH skips this epilogue).
//
// ABI: void CJ_MCC_ExclusiveScope(void* sret, void* fn, uintptr_t destSp)
//      SysV x86_64: rdi=sret, rsi=fn, rdx=destSp
//
// Callee-saved NOP: do not clobber rbx/r12–r15. Keep sret/fn in
// the arg regs (caller-saved) through bridge setup; hold stub FA in rax.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack):
//   [rbp]     saved rbp | return addr
//
// OS bridge frame:
//   [rbp]     saved rbp | return addr
//   -8..-24   N2CSlotData (GetSlotData = fa-24)
#define ExclusiveBridgeFrameSize    (8 * 4)

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

    // rdi=sret, rsi=fn stay in arg regs; rax = stub FA for bridge setup.
    movq    %rbp, %rax

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Red-zone / align for bridge frame on OS stack, then switch SP.
    subq    $128, %rdx
    andq    $-16, %rdx
    movq    %rdx, %rsp

    // Build OS bridge frame: saved rbp links to the CJ stub FA; return addr is glue.
    pushq   8(%rax)
    pushq   %rax
    movq    %rsp, %rbp
    subq    $ExclusiveBridgeFrameSize, %rsp

    // Fill N2CSlotData at fa-24 (do not clobber rdi/rsi).
    leaq    unwindPCForExclusiveStub(%rip), %rcx
    movq    %rcx, -24(%rbp)         // slot->pc
    movq    %rax, -16(%rbp)         // slot->fa
    movq    $2, -8(%rbp)            // slot->status = RISKY

    // AutoEnv Invoke: *(fn+8)(sret, fn, OuterTypeInfo=*(fn+0))
    // rdi/rsi still hold sret/fn from entry.
    movq    (%rsi), %rdx            // OuterTypeInfo
    movq    8(%rsi), %r11           // GENERIC_VIRTUAL_FUNC
    callq   *%r11
    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

    // Tear down bridge frame.
    movq    %rbp, %rsp
    popq    %rbp                    // restore CJ exclusive stub fa into %rbp
    addq    $8, %rsp                // discard bridge return address

    // Restore heap SP (stub FA) and return.
    movq    %rbp, %rsp
    popq    %rbp
    .cfi_def_cfa %rsp, 8
    retq

    .cfi_endproc
    .size CJ_MCC_ExclusiveScope, .-CJ_MCC_ExclusiveScope
