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
//      Entry (plain C, not LLVM sret): x0=sret, x1=fn, x2=destSp
//      AutoEnv call (Cangjie AArch64 sret): x8=sret, x0=fn, x1=*(fn+0)
//
// GC: sret must not live in callee-saved regs across AutoEnv (a safepoint may
// snapshot it as a GC root). Keep sret/fn in caller-saved regs through bridge
// setup (x0/x1), then reshape to x8/x0 before blr — still caller-saved. Hold
// stub FA in x3. Do not clobber x0/x1 during bridge setup.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack):
//   [x29+0x00] x29,x30
//   [x29+0x20] x19..x28   (RecordStubCalleeSaved)
#define ExclusiveScopeFrameSize     0x70

// OS bridge frame:
//   [x29+0x00] x29,x30
//   [x29+0x10] N2CSlotData (GetSlotData = fa+0x10)
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

    // x0=sret, x1=fn stay in arg regs; x3 = stub FA for bridge setup.
    mov     x3, x29

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Red-zone / align for bridge frame on OS stack, then switch SP.
    sub     x2, x2, #128
    bic     x2, x2, #15
    mov     sp, x2

    // Build OS bridge frame: saved fp links to the CJ stub FA; return addr is glue.
    stp     x29, x30, [sp, #-ExclusiveBridgeFrameSize]!
    mov     x29, sp

    // Fill N2CSlotData at fa+0x10 (do not clobber x0/x1).
    adrp    x4, unwindPCForExclusiveStub
    add     x4, x4, :lo12:unwindPCForExclusiveStub
    str     x4, [x29, #0x10]            // slot->pc
    str     x3, [x29, #0x18]            // slot->fa
    mov     x4, #2                      // RISKY
    str     x4, [x29, #0x20]            // slot->status

    // C → CJ AutoEnv ABI: x0=sret,x1=fn → x8=sret,x0=fn,x1=TI; blr *(fn+8).
    mov     x8, x0                      // sret (caller-saved)
    mov     x0, x1                      // this = AutoEnv
    ldp     x1, x9, [x0]                // TI=*(fn+0), GENERIC_VIRTUAL_FUNC=*(fn+8)
    blr     x9

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global unwindPCForExclusiveStubFull
unwindPCForExclusiveStubFull:

    // Tear down bridge frame.
    mov     sp, x29
    ldp     x29, x30, [sp], #ExclusiveBridgeFrameSize

    // Restore heap SP.
    mov     sp, x29

    // Restore callee-saved registers
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
