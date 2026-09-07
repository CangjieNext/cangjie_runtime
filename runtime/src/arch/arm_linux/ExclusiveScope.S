// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define cfi_adjust_cfa_offset(off)      .cfi_adjust_cfa_offset off
#define cfi_rel_offset(reg, off)        .cfi_rel_offset reg, off
#define cfi_restore(reg)                .cfi_restore reg
#define cfi_def_cfa_register(reg)       .cfi_def_cfa_register reg

// ------------------------------------------------------------------------------
// CJ_MCC_ExclusiveScope — ARM32
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
//      ARM32: r0=sret, r1=fn, r2=destSp
//
// Callee-saved NOP: do not clobber r4–r10. Keep sret/fn in the arg regs
// (caller-saved) through bridge setup.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack):
//   0x00  r11 (fp)
//   0x04  lr
//   0x08  padding
//
// OS bridge frame:
//   0x00  saved r11 = stub FA
//   0x04  lr = managed caller LR
//   0x08  padding
//
// 16-byte SP alignment is required for 128-bit NEON load/store.

#define ExclusiveScopeFrameSize     16

    .text
    .align 2
    .global CJ_MCC_ExclusiveScope
    .type CJ_MCC_ExclusiveScope, %function
CJ_MCC_ExclusiveScope:
    .cfi_startproc
    sub     sp, sp, #ExclusiveScopeFrameSize
    str     r11, [sp, #0]
    str     lr, [sp, #4]
    cfi_adjust_cfa_offset(ExclusiveScopeFrameSize)
    cfi_rel_offset(r11, 0)
    cfi_rel_offset(lr, 4)
    mov     r11, sp
    cfi_def_cfa_register(r11)

    // r0=sret, r1=fn stay in arg regs.

    // Red-zone / align for bridge frame on OS stack, then switch SP.
    sub     r2, r2, #64
    bic     r2, r2, #15
    mov     sp, r2

    // Build OS bridge frame: saved r11 = stub FA; lr = managed caller LR.
    sub     sp, sp, #ExclusiveScopeFrameSize
    str     r11, [sp, #0]
    str     lr, [sp, #4]
    mov     r11, sp

    // AutoEnv Invoke: *(fn+8)(sret, fn, OuterTypeInfo=*(fn+0))
    // ARM32 object: ti@0, pad@4, payload@8 → method0 at +8.
    // r0/r1 still hold sret/fn from entry.
    ldr     r2, [r1]                    // OuterTypeInfo
    ldr     r12, [r1, #8]               // GENERIC_VIRTUAL_FUNC
    blx     r12

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Tear down bridge frame.
    mov     sp, r11
    ldr     r11, [sp, #0]
    ldr     lr, [sp, #4]
    add     sp, sp, #ExclusiveScopeFrameSize

    // Restore heap SP and return.
    mov     sp, r11
    ldr     r11, [sp, #0]
    ldr     lr, [sp, #4]
    add     sp, sp, #ExclusiveScopeFrameSize
    cfi_adjust_cfa_offset(-ExclusiveScopeFrameSize)
    cfi_restore(r11)
    cfi_restore(lr)
    bx      lr

    .cfi_endproc
    .size CJ_MCC_ExclusiveScope, .-CJ_MCC_ExclusiveScope
