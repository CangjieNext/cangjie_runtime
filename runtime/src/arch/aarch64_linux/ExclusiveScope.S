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
// CJ_MCC_ExclusiveScope — AArch64
//
// Glue owns pin + protectAddr via CJ_MCC_ExclusiveScopeEnter/Leave; stub only
// when destSp != 0. Switches SP to destSp, builds OS bridge, invokes AutoEnv.
//
// ABI: void CJ_MCC_ExclusiveScope(void* sret, void* fn, uintptr_t destSp)
//      AArch64: x0=sret, x1=fn, x2=destSp
//
// GC: sret must not live in callee-saved regs across AutoEnv
// (a safepoint may snapshot it as a GC root). Keep sret/fn in x0/x1 through
// bridge setup; hold stub FA in x3. Do not clobber x0/x1 before the invoke.
// ------------------------------------------------------------------------------

// CJ exclusive stub (heap stack):
//   [x29+0x00] x29,x30
//   [x29+0x20] x19..x28   (RecordStubCalleeSaved)
//   [x29+0x70] d8..d15
//   [x29+0xb0] saved_sp   (Exception.cpp EXCLUSIVE_STUB_SAVED_CJ_SP_OFFSET)
#define ExclusiveScopeFrameSize     0xc0

// OS bridge:
//   [x29+0x00] x29,x30
//   [x29+0x10] N2CSlotData
// Callee-saved live on the CJ stub (RecordStubCalleeSaved); bridge records none.
#define ExclusiveBridgeFrameSize    48

    .text
    .align 2
    .global CJ_MCC_ExclusiveScope
    .type CJ_MCC_ExclusiveScope, @function
CJ_MCC_ExclusiveScope:
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

    // Callee-saved order must match RecordStubCalleeSaved (GC register roots).
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

    // x0=sret, x1=fn stay in arg regs; x3 = stub FA for bridge setup.
    mov     x3, x29

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Save CJ SP; switch to validated OS destSp (precondition: != 0).
    // Use x4/x5 only — must not clobber x0/x1.
    mov     x4, sp
    str     x4, [x29, #0xb0]
    mov     sp, x2
    sub     sp, sp, #128
    mov     x4, sp
    bic     x4, x4, #0xF
    mov     sp, x4

    // Bridge frame; x30 still holds return to glue (like saving real RA on x86).
    stp     x29, x30, [sp, #-ExclusiveBridgeFrameSize]!
    mov     x29, sp

    // Fill N2CSlotData at fa+0x10 (do not clobber x0/x1).
    adrp    x4, unwindPCForExclusiveStub
    add     x4, x4, :lo12:unwindPCForExclusiveStub
    str     x4, [x29, #0x10]            // slot->pc
    str     x3, [x29, #0x18]            // slot->fa
    mov     x4, #2                      // RISKY
    str     x4, [x29, #0x20]            // slot->status

    // AutoEnv Invoke: *(fn+8)(sret, fn, OuterTypeInfo=*(fn+0))
    // x0/x1 still hold sret/fn from entry.
    ldr     x2, [x1]                    // OuterTypeInfo
    ldr     x9, [x1, #8]                // GENERIC_VIRTUAL_FUNC
    blr     x9

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

    mov     sp, x29
    ldp     x29, x30, [sp], #ExclusiveBridgeFrameSize

    // Restore heap SP. Leave restores protectAddr in glue finally.
    ldr     x0, [x29, #0xb0]
    mov     sp, x0

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

    .cfi_endproc
    .size CJ_MCC_ExclusiveScope, .-CJ_MCC_ExclusiveScope
