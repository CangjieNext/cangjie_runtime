// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define cfi_adjust_cfa_offset(off)      .cfi_adjust_cfa_offset off
#define cfi_rel_offset(reg, off)        .cfi_rel_offset reg, off
#define cfi_restore(reg)                .cfi_restore reg
#define cfi_def_cfa_register(reg)       .cfi_def_cfa_register reg

#define CJTHREAD_THREAD_OFFSET      0x08
#define CONTEXT_R13SP               0x20
#define CONTEXT_FPSCR               0x6c

// ------------------------------------------------------------------------------
// Single-CJThread ExclusiveScope (redesign) — ARM32
//
// CJ exclusive stub + exclusive bridge with N2CSlotData cross-stack jump.
// exclusiveScopeCnt nests; skip-switch still builds a bridge; no saferegion.
// OS SP rejects null and SP < 0x1000 before switch (parity with x86).
// Callee-saved layouts must match RecordStubCalleeSaved /
// RecordExclusiveBridgeCalleeSaved (and arm RecordExclusiveStubCalleeSaved).
// ------------------------------------------------------------------------------

// CJ exclusive stub:
//   0x00  r11 (fp)
//   0x04  lr
//   0x10  r4..r10 (RecordStubCalleeSaved at fp+16)
//   0x2c  d8..d15
//   0x6c  executeClosure, closurePtr, saved_sp
#define ExclusiveScopeFrameSize     160

// OS bridge:
//   0x00  r11, lr
//   0x08  N2CSlotData (GetSlotData = fa+1 FrameAddress)
//   0x14  r4..r10 (RecordExclusiveBridgeCalleeSaved)
#define ExclusiveBridgeFrameSize    96

    .text
    .align 2
    .global CJ_MCC_ExclusiveScope
    .type CJ_MCC_ExclusiveScope, %function
CJ_MCC_ExclusiveScope:
    .cfi_startproc
    push    {r11, lr}
    cfi_adjust_cfa_offset(8)
    cfi_rel_offset(r11, 0)
    cfi_rel_offset(lr, 4)
    mov     r11, sp
    cfi_def_cfa_register(r11)
    sub     sp, sp, #(ExclusiveScopeFrameSize - 8)

    // Callee-saved at fp+16 (RecordStubCalleeSaved)
    str     r4, [r11, #0x10]
    str     r5, [r11, #0x14]
    str     r6, [r11, #0x18]
    str     r7, [r11, #0x1c]
    str     r8, [r11, #0x20]
    str     r9, [r11, #0x24]
    str     r10, [r11, #0x28]

    vstr    d8, [r11, #0x2c]
    vstr    d9, [r11, #0x34]
    vstr    d10, [r11, #0x3c]
    vstr    d11, [r11, #0x44]
    vstr    d12, [r11, #0x4c]
    vstr    d13, [r11, #0x54]
    vstr    d14, [r11, #0x5c]
    vstr    d15, [r11, #0x64]

    str     r0, [r11, #0x6c]            // executeClosure
    str     r1, [r11, #0x70]            // closurePtr
    mov     r0, #0
    str     r0, [r11, #0x74]            // saved_cj_sp: 0 => no switch

    // ExclusiveScopeState at [r11,#0x7c]: { cnt*, destSp }
    add     r0, r11, #0x7c
    bl      CJ_MCC_ExclusiveScopeEnter

    // destSp == 0: no CJ→OS switch and no bridge — invoke on the current stack.
    ldr     r0, [r11, #0x80]
    cmp     r0, #0
    beq     .L_exc_noswitch

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    str     sp, [r11, #0x74]            // saved_cj_sp
    mov     sp, r0
    sub     sp, sp, #64
    bic     sp, sp, #7

.L_exc_build_bridge:
    // Bridge on switched OS stack or current stack; r5 = CJ exclusive stub FA.
    mov     r5, r11                     // exclusive stub fa
    push    {r11, lr}
    mov     r11, sp
    sub     sp, sp, #(ExclusiveBridgeFrameSize - 8)

    // Callee-saved at fp+0x14 (RecordExclusiveBridgeCalleeSaved / arm exclusive)
    str     r4, [r11, #0x14]
    str     r5, [r11, #0x18]
    str     r6, [r11, #0x1c]
    str     r7, [r11, #0x20]
    str     r8, [r11, #0x24]
    str     r9, [r11, #0x28]
    str     r10, [r11, #0x2c]

    str     r5, [r11, #0x30]            // exclusive stub fa
    ldr     r0, [r5, #0x6c]
    ldr     r1, [r5, #0x70]
    str     r0, [r11, #0x34]
    str     r1, [r11, #0x38]

    // slot = { pc: unwindPCForExclusiveStub, fa: CJ stub, status: RISKY }
    mov     r0, r11                     // bridge fa
    mov     r1, r5                      // exclusive stub fa
    bl      MRT_SaveExclusiveBridgeSlot

    // Managed call: r0=sret, r2=closure (match prior Exclusive stub ABI)
    add     r0, r11, #0x40              // sret scratch on bridge
    mov     r1, #0
    ldr     r2, [r11, #0x38]            // closurePtr
    ldr     r4, [r11, #0x34]            // executeClosure
    ldr     r12, [r4, #8]
    mov     r3, #0
    blx     r12

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

    mov     r4, r0                      // preserve return

    ldr     r5, [r11, #0x18]
    ldr     r6, [r11, #0x1c]
    ldr     r7, [r11, #0x20]
    ldr     r8, [r11, #0x24]
    ldr     r9, [r11, #0x28]
    ldr     r10, [r11, #0x2c]

    mov     sp, r11
    pop     {r11, lr}

    // r11 is again the CJ exclusive stub. Restore heap SP only if we switched.
    ldr     r0, [r11, #0x74]
    cmp     r0, #0
    beq     .L_exc_after_stack_restore
    mov     sp, r0

.L_exc_after_stack_restore:
    mov     r5, r4
    add     r0, r11, #0x7c
    bl      CJ_MCC_ExclusiveScopeLeave
    mov     r0, r5
    // Fall through into epilogue (hot path).

.L_exc_cleanup:
    vldr    d8, [r11, #0x2c]
    vldr    d9, [r11, #0x34]
    vldr    d10, [r11, #0x3c]
    vldr    d11, [r11, #0x44]
    vldr    d12, [r11, #0x4c]
    vldr    d13, [r11, #0x54]
    vldr    d14, [r11, #0x5c]
    vldr    d15, [r11, #0x64]

    ldr     r4, [r11, #0x10]
    ldr     r5, [r11, #0x14]
    ldr     r6, [r11, #0x18]
    ldr     r7, [r11, #0x1c]
    ldr     r8, [r11, #0x20]
    ldr     r9, [r11, #0x24]
    ldr     r10, [r11, #0x28]

    mov     sp, r11
    pop     {r11, lr}
    cfi_adjust_cfa_offset(-8)
    cfi_restore(r11)
    cfi_restore(lr)
    bx      lr

// No stack switch (destSp == 0): invoke without a bridge frame.
.L_exc_noswitch:
    add     r0, r11, #0x78              // sret scratch
    mov     r1, #0
    ldr     r2, [r11, #0x70]            // closurePtr
    ldr     r4, [r11, #0x6c]            // executeClosure
    ldr     r12, [r4, #8]
    mov     r3, #0
    blx     r12
    mov     r4, r0
    // saved_cj_sp is still 0; join shared CntSub + epilogue.
    b       .L_exc_after_stack_restore

    .cfi_endproc
    .size CJ_MCC_ExclusiveScope, .-CJ_MCC_ExclusiveScope
