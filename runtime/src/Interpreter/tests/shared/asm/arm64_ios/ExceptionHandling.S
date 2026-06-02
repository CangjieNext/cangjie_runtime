    .arch armv8-a

    .set StubFrameContextSize,   (8 * 14)
    .set StubCalleeSaveAreaSize, (8 * 4)

    .text
    .align 4

    .extern _landingPad
    .weak_reference _landingPad
    .extern _g_GetCurrentCatchFunctionPCPtr

    .global _LandingPadWrapper
_LandingPadWrapper:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    adrp    x16, _g_GetCurrentCatchFunctionPCPtr@GOTPAGE
    ldr     x16, [x16, _g_GetCurrentCatchFunctionPCPtr@GOTPAGEOFF]
    ldr     x16, [x16]
    blr     x16

    str     x0, [x29, #8]

    ldp     x29, x30, [sp], #16
    b       _landingPad

    .extern _g_ThrowExceptionPtr
    .extern _g_GetThreadLocalDataPtr
    .extern _C2I_Stub
    .extern _C2I_Stub_End
    .extern _I2I_Stub
    .extern _I2I_Stub_End

    .global _unwindInterpretedFrameAndRethrowException
_unwindInterpretedFrameAndRethrowException:
    mov     x19, x0

    adrp    x20, _C2I_Stub@PAGE
    add     x20, x20, _C2I_Stub@PAGEOFF

    adrp    x21, _C2I_Stub_End@PAGE
    add     x21, x21, _C2I_Stub_End@PAGEOFF

    adrp    x22, _I2I_Stub@PAGE
    add     x22, x22, _I2I_Stub@PAGEOFF

    adrp    x23, _I2I_Stub_End@PAGE
    add     x23, x23, _I2I_Stub_End@PAGEOFF

.Lunwind_loop:
    cbz     x29, .Lcrash

    ldr     x10, [x29, #8]

    cmp     x10, x20
    blo     .Lcheck_i2i
    cmp     x10, x21
    blo     .Lc2i_stub_found

.Lcheck_i2i:
    cmp     x10, x22
    blo     .Lnot_stub
    cmp     x10, x23
    bhs     .Lnot_stub

.Li2i_stub_found:
    ldr     x29, [x29]
    ldr     x30, [x29, #8]
    add     sp,  x29, #16
    ldr     x29, [x29]

    mov     x0, x19
    b       .Ltail_call_throw

.Lc2i_stub_found:
    ldr     x29, [x29]

    adrp    x10, _g_GetThreadLocalDataPtr@GOTPAGE
    ldr     x10, [x10, _g_GetThreadLocalDataPtr@GOTPAGEOFF]
    ldr     x10, [x10]
    blr     x10
    mov     x28, x0

    mov     x0, x19

    ldp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    ldp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    ldp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    ldp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    ldr     x27,      [x29, #StubCalleeSaveAreaSize + 0x40]

    ldr     x30, [x29, #8]
    add     sp,  x29, #StubFrameContextSize
    ldr     x29, [x29]

    b       .Ltail_call_throw

.Ltail_call_throw:
    adrp    x10, _g_ThrowExceptionPtr@GOTPAGE
    ldr     x10, [x10, _g_ThrowExceptionPtr@GOTPAGEOFF]
    ldr     x10, [x10]
    br      x10

.Lnot_stub:
    ldr     x29, [x29]
    b       .Lunwind_loop

.Lcrash:
    udf     #0
