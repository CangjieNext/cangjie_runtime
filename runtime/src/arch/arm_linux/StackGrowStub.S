// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#define cfi_adjust_cfa_offset(off)	.cfi_adjust_cfa_offset off
#define cfi_rel_offset(reg, off)	.cfi_rel_offset reg, off
#define cfi_restore(reg)		.cfi_restore reg
#define cfi_def_cfa_register(reg)	.cfi_def_cfa_register reg

#define SignalStubFrameSize           (0x40)

// for arm32, do stack check and try to throw StackOverFlow Expection.

    .text
    .align 2
    .global CJ_MCC_StackGrowStub
    .type CJ_MCC_StackGrowStub, %function
CJ_MCC_StackGrowStub:
    .cfi_startproc
    sub  sp, sp, # SignalStubFrameSize
    cfi_adjust_cfa_offset (SignalStubFrameSize)
    str  r11, [sp]
    cfi_rel_offset (r11, 0)
    str  lr, [sp, #4]
    cfi_rel_offset (lr, 4)
    str  r12, [sp, #8]
    cfi_rel_offset (r12, 8)
    str  r3, [sp, #12]
    cfi_rel_offset (r3, 12)
    str  r2, [sp, #16]
    cfi_rel_offset (r2, 16)
    str  r1, [sp, #20]
    cfi_rel_offset (r1, 20)

    mov  r0, sp
    mov  r1, #0
    mov  r2, lr
    bl  MRT_StackGrow
    .global unwindPCForStackGrowStub
unwindPCForStackGrowStub:
    ldr  r11, [sp]
    cfi_restore(r11)
    ldr  lr, [sp, #4]
    cfi_restore(lr)
    ldr  r12, [sp, #8]
    cfi_restore(r12)
    ldr  r3, [sp, #12]
    cfi_restore(r3)
    ldr  r2, [sp, #16]
    cfi_restore(r2)
    ldr  r1, [sp, #20]
    cfi_restore(r1)
    add  sp, sp, # SignalStubFrameSize
    cfi_adjust_cfa_offset (-SignalStubFrameSize)
    bx  lr
    .cfi_endproc
    .size CJ_MCC_StackGrowStub, .-CJ_MCC_StackGrowStub

