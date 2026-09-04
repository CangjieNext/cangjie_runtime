// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ------------------------------------------------------------------------------
// CJ_MCC_ExclusiveScope — Windows x64
//
// CodeGen owns pin + protectAddr via CJ_MCC_ExclusiveScopeEnter/Leave and
// only calls this stub when destSp != 0. This stub:
//   1) builds the CJ exclusive stub frame on the heap stack
//   2) switches SP to destSp
//   3) builds the exclusive OS bridge and invokes the AutoEnv closure
//
// protectAddr and exclusiveScopeCnt are owned by Enter/Leave in exclusiveScopeImpl's finally
// (EH skips this epilogue).
//
// ABI: void CJ_MCC_ExclusiveScope(void* sret, void* fn, uintptr_t destSp)
//      Win64: rcx=sret, rdx=fn, r8=destSp
//
// Callee-saved NOP: do not clobber rbx/rdi/rsi/r12–r15. Keep sret/fn in the
// arg regs (volatile) through bridge setup; hold stub FA in rax.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack):
//   [rbp]     saved rbp | return addr
//
// OS bridge frame:
//   [rbp]     saved rbp | return addr
//   -8..-24   N2CSlotData (GetSlotData = fa-24)
//             slot->{pc,fa} = managed caller.
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
    .seh_endprologue

    // rcx=sret, rdx=fn stay in arg regs; rax = stub FA for bridge setup.
    movq    %rbp, %rax

    // Red-zone / align for bridge frame on OS stack, then switch SP.
    subq    $128, %r8
    andq    $-16, %r8
    movq    %r8, %rsp

    // Build OS bridge frame: saved rbp links to the CJ stub FA; return addr is the managed caller's.
    pushq   8(%rax)
    pushq   %rax
    movq    %rsp, %rbp
    subq    $ExclusiveBridgeFrameSize, %rsp

    // Fill N2CSlotData at fa-24 (do not clobber rcx/rdx).
    // slot->{pc,fa} = managed caller RA/FA so the walker skips the stub frame.
    movq    8(%rax), %r9
    movq    %r9, -24(%rbp)          // slot->pc  = managed caller RA
    movq    (%rax), %r9
    movq    %r9, -16(%rbp)          // slot->fa  = managed caller FA
    movq    $2, -8(%rbp)            // slot->status = RISKY

    // AutoEnv Invoke: *(fn+8)(sret, fn, OuterTypeInfo=*(fn+0))
    // Win64: rcx=sret, rdx=this, r8=OuterTypeInfo; need 32-byte shadow.
    // rcx/rdx still hold sret/fn from entry.
    movq    (%rdx), %r8             // OuterTypeInfo
    movq    8(%rdx), %r11           // GENERIC_VIRTUAL_FUNC
    subq    $32, %rsp
    callq   *%r11
    addq    $32, %rsp

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Tear down bridge frame.
    movq    %rbp, %rsp
    popq    %rbp                    // restore CJ exclusive stub fa into %rbp
    addq    $8, %rsp                // discard bridge return address

    // Restore heap SP (stub FA) and return.
    movq    %rbp, %rsp
    popq    %rbp
    retq

    .seh_endproc
