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
// Glue owns pin + protectAddr via CJ_MCC_ExclusiveScopeEnter/Leave; stub only
// when destSp != 0. Switches SP to destSp, builds OS bridge, invokes AutoEnv.
//
// ABI: void CJ_MCC_ExclusiveScope(void* sret, void* fn, uintptr_t destSp)
//      ARM32: r0=sret, r1=fn, r2=destSp
//
// GC: sret must not live in callee-saved regs across AutoEnv
// (a safepoint may snapshot it as a GC root). Keep sret/fn in r0/r1 through
// bridge setup; hold stub FA in r3. Do not clobber r0/r1 before the invoke.
// ------------------------------------------------------------------------------

// CJ exclusive stub:
//   0x00  r11 (fp)
//   0x04  lr
//   0x10  r4..r10 (RecordStubCalleeSaved at fp+16)
//   0x2c  d8..d15
//   0x6c  saved_sp   (Exception.cpp EXCLUSIVE_STUB_SAVED_CJ_SP_OFFSET)
#define ExclusiveScopeFrameSize     0x70

// OS bridge:
//   0x00  r11, lr
//   0x08  N2CSlotData
// Callee-saved live on the CJ stub (RecordStubCalleeSaved); bridge records none.
#define ExclusiveBridgeFrameSize    32

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

    // r0=sret, r1=fn stay in arg regs; r3 = stub FA for bridge setup.
    mov     r3, r11

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Save CJ SP; switch to validated OS destSp (precondition: != 0).
    str     sp, [r11, #0x6c]
    mov     sp, r2
    sub     sp, sp, #64
    bic     sp, sp, #7

    // Bridge on OS stack; lr still holds return to glue.
    push    {r11, lr}
    mov     r11, sp
    sub     sp, sp, #(ExclusiveBridgeFrameSize - 8)

    // Fill N2CSlotData at fa+0x08 (do not clobber r0/r1).
    adr     r12, unwindPCForExclusiveStub
    str     r12, [r11, #0x08]           // slot->pc
    str     r3, [r11, #0x0c]            // slot->fa
    mov     r12, #2                     // RISKY
    str     r12, [r11, #0x10]           // slot->status

    // AutoEnv Invoke: *(fn+8)(sret, fn, OuterTypeInfo=*(fn+0))
    // ARM32 object: ti@0, pad@4, payload@8 → method0 at +8.
    // r0/r1 still hold sret/fn from entry.
    ldr     r2, [r1]                    // OuterTypeInfo
    ldr     r12, [r1, #8]               // GENERIC_VIRTUAL_FUNC
    blx     r12

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

    mov     sp, r11
    pop     {r11, lr}

    // Restore heap SP. Leave restores protectAddr in glue finally.
    ldr     r0, [r11, #0x6c]
    mov     sp, r0

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

    .cfi_endproc
    .size CJ_MCC_ExclusiveScope, .-CJ_MCC_ExclusiveScope
