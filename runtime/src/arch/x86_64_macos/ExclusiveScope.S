// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ------------------------------------------------------------------------------
// _CJ_MCC_ExclusiveScope
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
// ABI: void _CJ_MCC_ExclusiveScope(void* sret, void* fn, uintptr_t destSp)
//      SysV x86_64: rdi=sret, rsi=fn, rdx=destSp
//
// Callee-saved NOP: do not clobber rbx/r12–r15. Keep sret/fn in
// the arg regs (caller-saved) through bridge setup.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack):
//   [rbp]     saved rbp | return addr
//
// OS bridge frame:
//   [rbp]  saved rbp = stub FA | return addr = managed caller RA

    .text
    .align 2
    .global _CJ_MCC_ExclusiveScope
    .cfi_sections .debug_frame
_CJ_MCC_ExclusiveScope:
    .cfi_startproc

    pushq   %rbp
    .cfi_def_cfa_offset 16
    .cfi_offset %rbp, -16
    movq    %rsp, %rbp
    .cfi_def_cfa_register %rbp

    // rdi=sret, rsi=fn stay in arg regs.

    // Red-zone / align for bridge frame on OS stack, then switch SP.
    subq    $128, %rdx
    andq    $-16, %rdx
    movq    %rdx, %rsp

    // Build OS bridge frame: saved rbp = stub FA; RA = managed caller.
    pushq   8(%rbp)
    pushq   %rbp
    movq    %rsp, %rbp

    // AutoEnv Invoke: *(fn+8)(sret, fn, OuterTypeInfo=*(fn+0))
    // rdi/rsi still hold sret/fn from entry.
    movq    (%rsi), %rdx            // OuterTypeInfo
    movq    8(%rsi), %r11           // GENERIC_VIRTUAL_FUNC
    callq   *%r11
    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global _unwindPCForExclusiveStub
_unwindPCForExclusiveStub:

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
