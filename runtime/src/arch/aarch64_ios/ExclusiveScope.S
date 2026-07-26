// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define cfi_adjust_cfa_offset(off)      .cfi_adjust_cfa_offset off
#define cfi_rel_offset(reg, off)        .cfi_rel_offset reg, off
#define cfi_restore(reg)                .cfi_restore reg
#define cfi_def_cfa_register(reg)       .cfi_def_cfa_register reg

#define CJTHREAD_THREAD_OFFSET      0x10
#define CONTEXT_SP                  0x70
#define CONTEXT_FPCR                0xb8

// ------------------------------------------------------------------------------
// Single-CJThread ExclusiveScope (redesign) — AArch64 iOS
//
// Same redesign as aarch64_linux: CJ stub + exclusive bridge with N2CSlotData
// cross-stack jump; exclusiveScopeCnt nesting; skip-switch still builds bridge;
// no saferegion. OS SP rejects null and SP < 0x1000 before switch.
// Symbols are underscored for Mach-O.
// ------------------------------------------------------------------------------

// CJ exclusive stub (heap stack, or current stack when switch is skipped):
//   [x29+0x00] x29,x30
//   [x29+0x20] x19..x28   (RecordStubCalleeSaved)
//   [x29+0x70] d8..d15
//   [x29+0xc0] executeClosure, closurePtr, saved_sp, thread/ctx
#define ExclusiveScopeFrameSize     (16 * 15)

// OS bridge:
//   [x29+0x00] x29,x30
//   [x29+0x10] N2CSlotData (GetSlotData = fa+1 FrameAddress)
//   [x29+0x30] x19..x28   (RecordExclusiveBridgeCalleeSaved)
//   [x29+0x80] executeClosure, closurePtr, exclusive stub fa
#define ExclusiveBridgeFrameSize    256

    .text
    .align 2
    .global _CJ_MCC_ExclusiveScope
    .type _CJ_MCC_ExclusiveScope, @function
_CJ_MCC_ExclusiveScope:
    .cfi_startproc
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    paciasp
#endif
    stp     x29, x30, [sp, #-ExclusiveScopeFrameSize]!
    cfi_adjust_cfa_offset(ExclusiveScopeFrameSize)
    cfi_rel_offset(x29, 0)
    cfi_rel_offset(x30, 8)
    mov     x29, sp
    cfi_def_cfa_register(x29)

    // Save callee-saved registers (RecordStubCalleeSaved / GC roots from x29+0x20)
    stp     x19, x20, [x29, #0x20]
    cfi_rel_offset(x19, 0x20)
    cfi_rel_offset(x20, 0x28)
    stp     x21, x22, [x29, #0x30]
    cfi_rel_offset(x21, 0x30)
    cfi_rel_offset(x22, 0x38)
    stp     x23, x24, [x29, #0x40]
    cfi_rel_offset(x23, 0x40)
    cfi_rel_offset(x24, 0x48)
    stp     x25, x26, [x29, #0x50]
    cfi_rel_offset(x25, 0x50)
    cfi_rel_offset(x26, 0x58)
    stp     x27, x28, [x29, #0x60]
    cfi_rel_offset(x27, 0x60)
    cfi_rel_offset(x28, 0x68)

    stp     d8, d9, [x29, #0x70]
    stp     d10, d11, [x29, #0x80]
    stp     d12, d13, [x29, #0x90]
    stp     d14, d15, [x29, #0xa0]

    mov     x19, x0                     // executeClosure
    mov     x20, x1                     // closurePtr
    str     x19, [x29, #0xc0]
    str     x20, [x29, #0xc8]
    str     xzr, [x29, #0xd0]           // saved_sp: 0 => no switch

    // Pin OS thread; ==1 means outermost entry.
    // ExclusiveScopeState at [x29,#0xd8]: { cnt*, destSp }
    add     x0, x29, #0xd8
    bl      _CJ_MCC_ExclusiveScopeEnter

    // destSp == 0: no CJ→OS switch and no bridge — invoke on the current stack.
    ldr     x0, [x29, #0xe0]
    cbz     x0, .L_exc_noswitch

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global _unwindPCForExclusiveStub
_unwindPCForExclusiveStub:

    mov     x1, sp
    str     x1, [x29, #0xd0]
    mov     sp, x0
    sub     sp, sp, #128
    mov     x0, sp
    bic     x0, x0, #0xF
    mov     sp, x0
    ldr     w0, [x21, #CONTEXT_FPCR]
    msr     fpcr, x0
    sub     sp, sp, #128
    mov     x0, sp
    bic     x0, x0, #0xF
    mov     sp, x0

.L_exc_build_bridge:
    // Bridge frame; x22 keeps CJ exclusive stub FA across the managed call.
    mov     x22, x29

    stp     x29, x30, [sp, #-ExclusiveBridgeFrameSize]!
    mov     x29, sp

    // Callee-saved at +0x30 for RecordExclusiveBridgeCalleeSaved
    stp     x19, x20, [x29, #0x30]
    stp     x21, x22, [x29, #0x40]
    stp     x23, x24, [x29, #0x50]
    stp     x25, x26, [x29, #0x60]
    stp     x27, x28, [x29, #0x70]

    // Args live on the CJ stub frame (may be a different stack than sp now).
    str     x22, [x29, #0x80]           // exclusive stub fa
    ldr     x19, [x22, #0xc0]           // executeClosure from CJ frame
    ldr     x20, [x22, #0xc8]           // closurePtr
    str     x19, [x29, #0x88]
    str     x20, [x29, #0x90]

    // slot = { pc: unwindPCForExclusiveStub, fa: CJ stub, status: RISKY }
    mov     x0, x29                     // bridge fa
    mov     x1, x22                     // exclusive stub fa
    bl      _MRT_SaveExclusiveBridgeSlot

    // Managed call: Future.executeClosure ABI — x8/x0=sret, x1=closure
    add     x8, x29, #0xa8              // sret scratch
    mov     x0, x8
    ldr     x1, [x29, #0x90]            // closurePtr
    ldr     x19, [x29, #0x88]           // executeClosure
    ldr     x9, [x19, #8]
    blr     x9

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global _unwindPCForExclusiveStubFull
_unwindPCForExclusiveStubFull:

    mov     x19, x0                     // preserve return

    // Restore bridge callee-saved (except x19 = return)
    ldr     x20, [x29, #0x38]
    ldp     x21, x22, [x29, #0x40]
    ldp     x23, x24, [x29, #0x50]
    ldp     x25, x26, [x29, #0x60]
    ldp     x27, x28, [x29, #0x70]

    mov     sp, x29
    ldp     x29, x30, [sp], #ExclusiveBridgeFrameSize

    // x29 is again the CJ exclusive stub. Restore heap SP only if we switched.
    ldr     x0, [x29, #0xd0]
    cbz     x0, .L_exc_after_stack_restore
    mov     sp, x0

.L_exc_after_stack_restore:
    mov     x20, x19                    // return value
    add     x0, x29, #0xd8
    bl      _CJ_MCC_ExclusiveScopeLeave
    mov     x0, x20
    // Fall through into epilogue (hot path).

.L_exc_cleanup:
    ldp     d8, d9, [x29, #0x70]
    ldp     d10, d11, [x29, #0x80]
    ldp     d12, d13, [x29, #0x90]
    ldp     d14, d15, [x29, #0xa0]

    ldp     x19, x20, [x29, #0x20]
    cfi_restore(x19)
    cfi_restore(x20)
    ldp     x21, x22, [x29, #0x30]
    cfi_restore(x21)
    cfi_restore(x22)
    ldp     x23, x24, [x29, #0x40]
    cfi_restore(x23)
    cfi_restore(x24)
    ldp     x25, x26, [x29, #0x50]
    cfi_restore(x25)
    cfi_restore(x26)
    ldp     x27, x28, [x29, #0x60]
    cfi_restore(x27)
    cfi_restore(x28)

    ldp     x29, x30, [sp], #ExclusiveScopeFrameSize
    cfi_adjust_cfa_offset(-ExclusiveScopeFrameSize)
    cfi_restore(x29)
    cfi_restore(x30)

#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    autiasp
#endif
    ret

// No stack switch (destSp == 0): invoke without a bridge frame.
.L_exc_noswitch:
    add     x8, x29, #0xb0              // sret scratch
    mov     x0, x8
    ldr     x1, [x29, #0xc8]            // closurePtr
    ldr     x19, [x29, #0xc0]           // executeClosure
    ldr     x9, [x19, #8]
    blr     x9
    mov     x19, x0
    // saved_sp is still 0; join shared CntSub + epilogue.
    b       .L_exc_after_stack_restore


    .cfi_endproc
    .size _CJ_MCC_ExclusiveScope, .-_CJ_MCC_ExclusiveScope
