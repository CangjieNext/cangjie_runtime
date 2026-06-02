    .arch armv8-a

    .set VIRTUAL_FUNCS_N, 10

    // Frame layout mirrors CalleeSavedStub.S so that RecordStubCalleeSaved
    // can trace GC roots held in callee-saved registers.
    .set StubFrameContextSize,   (8 * 14)
    .set StubCalleeSaveAreaSize, (8 * 4)

    .extern _virtualFuncs
    .extern _g_getPendingExceptionPtr

    .macro trampoline_entry idx
    adrp    x16, _virtualFuncs@GOTPAGE
    ldr     x16, [x16, _virtualFuncs@GOTPAGEOFF]
    ldr     x0,  [x16, #(\idx * 8)]
    b       _C2I_Stub
    .endm

    .macro trampoline_table count, idx=1
    .if \idx <= \count
    trampoline_entry \idx
    trampoline_table \count, (\idx + 1)
    .endif
    .endm

    .text
    .align 4

    .global _trampolinesStart
_trampolinesStart:
    trampoline_table VIRTUAL_FUNCS_N

    .global _C2I_Stub
    .extern _g_CJ_MCC_StackGrowStubPtr
    .extern _g_GetThreadLocalDataPtr
    .extern _g_InterpreterStackReserveSize
    .align 4
_C2I_Stub:
    // x0  = CALLEE_ADDR (interpreter function, set by trampoline).
    // x28 = thread-local data.

    stp     x29, x30, [sp, #-StubFrameContextSize]!
    mov     x29, sp

    // Stack overflow check: ThreadLocalData::protectAddr is at offset 40.
    ldr     x9,  [x28, #40]
    adrp    x10, _g_InterpreterStackReserveSize@GOTPAGE
    ldr     x10, [x10, _g_InterpreterStackReserveSize@GOTPAGEOFF]
    ldr     x10, [x10]
    add     x9,  x9, x10
    cmp     sp,  x9
    bls     .Lstack_overflow

.Lstack_check_end:
    stp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    stp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    stp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    stp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    stp     x27, x28, [x29, #StubCalleeSaveAreaSize + 0x40]

.Lcall_interpreter:
    mov     x9,  x0
    mov     x0,  x28
    blr     x9

    // Preserve the interpreter result while refreshing thread-local data.
    str     x0,  [x29, #StubCalleeSaveAreaSize + 0x48]

    adrp    x10, _g_GetThreadLocalDataPtr@GOTPAGE
    ldr     x10, [x10, _g_GetThreadLocalDataPtr@GOTPAGEOFF]
    ldr     x10, [x10]
    blr     x10
    mov     x28, x0

    ldp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    ldp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    ldp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    ldp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    ldr     x27,      [x29, #StubCalleeSaveAreaSize + 0x40]

    ldr     x0,  [x29, #StubCalleeSaveAreaSize + 0x48]

    ldp     x29, x30, [sp], #StubFrameContextSize
    ret

.Lstack_overflow:
    stp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    stp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    stp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    stp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    stp     x27, x28, [x29, #StubCalleeSaveAreaSize + 0x40]

    mov     x9,  #StubFrameContextSize
    adrp    x10, _g_InterpreterStackReserveSize@GOTPAGE
    ldr     x10, [x10, _g_InterpreterStackReserveSize@GOTPAGEOFF]
    ldr     x10, [x10]
    add     x9,  x9,  x10

    adrp    x10, _g_CJ_MCC_StackGrowStubPtr@GOTPAGE
    ldr     x10, [x10, _g_CJ_MCC_StackGrowStubPtr@GOTPAGEOFF]
    ldr     x10, [x10]
    blr     x10

    str     x0,  [sp, #-16]!
    adrp    x10, _g_getPendingExceptionPtr@GOTPAGE
    ldr     x10, [x10, _g_getPendingExceptionPtr@GOTPAGEOFF]
    ldr     x10, [x10]
    blr     x10
    mov     x11, x0
    ldr     x0,  [sp], #16
    cbnz    x11, .Lsoe_return

    b       .Lcall_interpreter

.Lsoe_return:
    ldp     x29, x30, [sp], #StubFrameContextSize
    ret

    .global _C2I_Stub_End
_C2I_Stub_End:
    nop
