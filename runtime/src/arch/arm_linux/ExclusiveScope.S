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
//      ARM32: r0=sret, r1=fn, r2=destSp
//
// GC: sret must not live in callee-saved regs across AutoEnv
// (a safepoint may snapshot it as a GC root). Keep sret/fn in the arg regs
// (caller-saved) through bridge setup; hold stub FA in r3. Do not clobber
// r0/r1 before the invoke.
// ------------------------------------------------------------------------------

// CJ exclusive stub frame (heap stack):
//   0x00  r11 (fp)
//   0x04  lr
//   0x10  r4..r10 (RecordStubCalleeSaved)
#define ExclusiveScopeFrameSize     0x30

// OS bridge frame:
//   0x00  r11, lr
//   0x08  N2CSlotData (GetSlotData = fa+0x08)
// Callee-saved live on the CJ stub (RecordStubCalleeSaved); bridge records none.
#define ExclusiveBridgeFrameSize    32

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

    // Callee-saved order must match RecordStubCalleeSaved (GC register roots).
    str     r4, [r11, #0x10]
    cfi_rel_offset(r4, 0x10)
    str     r5, [r11, #0x14]
    cfi_rel_offset(r5, 0x14)
    str     r6, [r11, #0x18]
    cfi_rel_offset(r6, 0x18)
    str     r7, [r11, #0x1c]
    cfi_rel_offset(r7, 0x1c)
    str     r8, [r11, #0x20]
    cfi_rel_offset(r8, 0x20)
    str     r9, [r11, #0x24]
    cfi_rel_offset(r9, 0x24)
    str     r10, [r11, #0x28]
    cfi_rel_offset(r10, 0x28)

    // r0=sret, r1=fn stay in arg regs; r3 = stub FA for bridge setup.
    mov     r3, r11

    // IP stored into the bridge N2CSlotData so stack walk lands on this stub.
    .global unwindPCForExclusiveStub
unwindPCForExclusiveStub:

    // Red-zone / align for bridge frame on OS stack, then switch SP.
    sub     r2, r2, #64
    bic     r2, r2, #15
    mov     sp, r2

    // Build OS bridge frame: saved fp links to the CJ stub FA; return addr is glue.
    sub     sp, sp, #ExclusiveBridgeFrameSize
    str     r11, [sp, #0]
    str     lr, [sp, #4]
    mov     r11, sp

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

    // Tear down bridge frame.
    mov     sp, r11
    ldr     r11, [sp, #0]
    ldr     lr, [sp, #4]
    add     sp, sp, #ExclusiveBridgeFrameSize

    // Restore heap SP.
    mov     sp, r11

    // Restore callee-saved registers
    ldr     r4, [r11, #0x10]
    cfi_restore(r4)
    ldr     r5, [r11, #0x14]
    cfi_restore(r5)
    ldr     r6, [r11, #0x18]
    cfi_restore(r6)
    ldr     r7, [r11, #0x1c]
    cfi_restore(r7)
    ldr     r8, [r11, #0x20]
    cfi_restore(r8)
    ldr     r9, [r11, #0x24]
    cfi_restore(r9)
    ldr     r10, [r11, #0x28]
    cfi_restore(r10)

    ldr     r11, [sp, #0]
    ldr     lr, [sp, #4]
    add     sp, sp, #ExclusiveScopeFrameSize
    cfi_adjust_cfa_offset(-ExclusiveScopeFrameSize)
    cfi_restore(r11)
    cfi_restore(lr)
    bx      lr

    .cfi_endproc
    .size CJ_MCC_ExclusiveScope, .-CJ_MCC_ExclusiveScope
