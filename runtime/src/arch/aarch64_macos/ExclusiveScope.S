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
// _CJ_MCC_ExclusiveScope — AArch64
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
//      Entry (plain C, not LLVM sret): x0=sret, x1=fn, x2=destSp
//      AutoEnv call (Cangjie AArch64 sret): x8=sret, x0=fn, x1=*(fn+0)
//
// Callee-saved NOP: do not clobber x19–x28. Keep sret/fn in
// caller-saved regs through bridge setup (x0/x1), then reshape to x8/x0
// before blr — still caller-saved. Hold stub FA in x3. Do not clobber
// x0/x1 during bridge setup.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack):
//   [x29+0x00] x29,x30
#define ExclusiveScopeFrameSize     16

// OS bridge frame:
//   [x29+0x00] x29,x30
//   [x29+0x10] N2CSlotData (GetSlotData = fa+0x10)
//              slot->{pc,fa} = managed caller.
#define ExclusiveBridgeFrameSize    48

    .text
    .align 2
    .global _CJ_MCC_ExclusiveScope
    .cfi_sections .debug_frame
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

    // x0=sret, x1=fn stay in arg regs; x3 = stub FA for bridge setup.
    mov     x3, x29

    // Red-zone / align for bridge frame on OS stack, then switch SP.
    sub     x2, x2, #128
    bic     x2, x2, #15
    mov     sp, x2

    // Build OS bridge frame: saved fp links to the CJ stub FA; return addr is the managed caller's.
    stp     x29, x30, [sp, #-ExclusiveBridgeFrameSize]!
    mov     x29, sp

    // Fill N2CSlotData at fa+0x10 (do not clobber x0/x1).
    // slot->{pc,fa} = managed caller LR/FA so the walker skips the stub frame.
    ldr     x4, [x3, #8]
    str     x4, [x29, #0x10]            // slot->pc  = managed caller LR
    ldr     x4, [x3]
    str     x4, [x29, #0x18]            // slot->fa  = managed caller FA
    mov     x4, #2                      // RISKY
    str     x4, [x29, #0x20]            // slot->status

    // C → CJ AutoEnv ABI: x0=sret,x1=fn → x8=sret,x0=fn,x1=TI; blr *(fn+8).
    mov     x8, x0                      // sret (caller-saved)
    mov     x0, x1                      // this = AutoEnv
    ldp     x1, x9, [x0]                // TI=*(fn+0), GENERIC_VIRTUAL_FUNC=*(fn+8)
    blr     x9

    // Post-call PC: exclusive bridge identity for IsExclusiveBridgeFrame.
    .global _unwindPCForExclusiveStub
_unwindPCForExclusiveStub:

    // Tear down bridge frame.
    mov     sp, x29
    ldp     x29, x30, [sp], #ExclusiveBridgeFrameSize

    // Restore heap SP and return.
    mov     sp, x29
    ldp     x29, x30, [sp], #ExclusiveScopeFrameSize
    cfi_adjust_cfa_offset(-ExclusiveScopeFrameSize)
    cfi_restore(x29)
    cfi_restore(x30)

#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    autiasp
#endif
    ret

    .cfi_endproc
