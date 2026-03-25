    .arch armv8-a

    // C2I_Stub frame constants — must match C2I_Stub.S exactly.
    .set StubFrameContextSize,   (8 * 14)   // 112 bytes
    .set StubCalleeSaveAreaSize, (8 * 4)    //  32 bytes

    .text
    .align 4

    // =========================================================================
    // LandingPadWrapper
    //
    // Jumped to (br) by RestoreContextForEH after exception context has
    // been restored. Manually constructs a frame for landingPad so that when
    // landingPad executes its final `ret` it returns to the catch-block PC
    // supplied by g_GetCurrentCatchFunctionPCPtr.
    // =========================================================================

    .extern landingPad
    .weak   landingPad
    .extern g_GetCurrentCatchFunctionPCPtr

    .global LandingPadWrapper
LandingPadWrapper:
    // Build a minimal frame (x30 will be overwritten with the target PC below).
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    adrp    x16, :got:g_GetCurrentCatchFunctionPCPtr
    ldr     x16, [x16, :got_lo12:g_GetCurrentCatchFunctionPCPtr]
    ldr     x16, [x16]
    blr     x16             // x0 = target PC for after landingPad

    // Overwrite the saved x30 slot so that the ldp below restores x30 to the catch-block PC. 
    // When landingPad returns via `ret`, control transfers directly to that PC.
    str     x0, [x29, #8]

    ldp     x29, x30, [sp], #16     // x29 restored; x30 = catch-block PC

    b       landingPad              // tail-jump; landingPad will return to x30


    // =========================================================================
    // unwindInterpretedFrameAndRethrowException
    //
    // Dynamically unwinds frames until a C2I_Stub or I2I_Stub frame is found,
    // restores the appropriate callee-saved state, then re-throws the exception
    // as if it had been thrown from the stub's caller.
    //
    // Arguments:
    //   x0 = exception_obj
    //
    // This function is __attribute__((noreturn)); it never returns to its caller.
    // It has no prologue because we must not disturb the frame chain.
    // Callee-saved registers x19-x23 are used freely since the function never returns.
    // =========================================================================

    .extern g_ThrowExceptionPtr
    .extern g_GetThreadLocalDataPtr
    .extern C2I_Stub
    .extern C2I_Stub_End
    .extern I2I_Stub
    .extern I2I_Stub_End

    .global unwindInterpretedFrameAndRethrowException
unwindInterpretedFrameAndRethrowException:
    // Preserve exception_obj across calls and frame walks.
    mov     x19, x0

    // Load stub address ranges once.
    adrp    x20, :got:C2I_Stub
    ldr     x20, [x20, :got_lo12:C2I_Stub]

    adrp    x21, :got:C2I_Stub_End
    ldr     x21, [x21, :got_lo12:C2I_Stub_End]

    adrp    x22, :got:I2I_Stub
    ldr     x22, [x22, :got_lo12:I2I_Stub]

    adrp    x23, :got:I2I_Stub_End
    ldr     x23, [x23, :got_lo12:I2I_Stub_End]

.Lunwind_loop:
    // If x29 is 0 we have walked off the top of the thread stack.
    cbz     x29, .Lcrash

    // each standard frame stores:
    //   [x29 + 0] = saved x29 (previous frame pointer)
    //   [x29 + 8] = saved x30 (return address into the caller)
    ldr     x10, [x29, #8]      // x10 = return address of this frame

    // Check whether the return address falls inside [C2I_Stub, C2I_Stub_End)
    cmp     x10, x20
    blo     .Lcheck_i2i         // below C2I_Stub → not in C2I range
    cmp     x10, x21
    blo     .Lc2i_stub_found    // below C2I_Stub_End → inside C2I range

.Lcheck_i2i:
    cmp     x10, x22
    blo     .Lnot_stub          // below I2I_Stub → not in I2I range
    cmp     x10, x23
    bhs     .Lnot_stub          // at or above I2I_Stub_End → not in I2I range

    // =========================================================================
    // I2I_Stub frame found: the current frame was called from inside I2I_Stub.
    // I2I_Stub frame size = 16 bytes; no callee-saved registers to restore.
    // =========================================================================
.Li2i_stub_found:
    // Step into I2I_Stub's frame.
    ldr     x29, [x29]          // x29 = I2I_Stub's saved x29

    // Load the return address that I2I_Stub returns to (its caller's address).
    ldr     x30, [x29, #8]

    // Restore sp to the state before I2I_Stub's frame (frame = 16 bytes).
    add     sp,  x29, #16

    // Restore x29 to I2I_Stub's caller's frame pointer.
    ldr     x29, [x29]

    mov     x0, x19             // arg0 = exception_obj (x19 intact on I2I path)
    b       .Ltail_call_throw

    // =========================================================================
    // C2I_Stub frame found: the current frame was called from inside C2I_Stub.
    //
    // On entry x29 points to the frame that was called from C2I_Stub.
    // We step into C2I_Stub's own frame, refresh the TLD, restore all
    // callee-saved registers (x19-x28), then prepare a tail call to
    // g_ThrowExceptionPtr so the exception surfaces in C2I_Stub's caller.
    // =========================================================================
.Lc2i_stub_found:
    // Step into C2I_Stub's frame.
    ldr     x29, [x29]          // x29 = C2I_Stub's saved x29

    // Refresh the TLD: the thread-local data pointer may have changed after GC.
    // x19-x23 (exception_obj + stub ranges) are callee-saved and survive the call.
    adrp    x10, :got:g_GetThreadLocalDataPtr
    ldr     x10, [x10, :got_lo12:g_GetThreadLocalDataPtr]
    ldr     x10, [x10]
    blr     x10                 // x0 = current thread-local data
    mov     x28, x0             // x28 = refreshed EE

    // move exception_obj from x19 into x0 before the callee-register
    // restore below overwrites x19 with C2I_Stub's saved value of x19.
    mov     x0, x19             // x0 = exception_obj

    // Restore x19-x27 from C2I_Stub's frame.
    // [x29 + 104] holds the old x28; skip it — x28 was just refreshed above.
    ldp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    ldp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    ldp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    ldp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    ldr     x27,      [x29, #StubCalleeSaveAreaSize + 0x40]

    // Prepare for tail call: x30 = return address to C2I_Stub's caller.
    ldr     x30, [x29, #8]

    // Unwind C2I_Stub's frame (StubFrameContextSize bytes) and restore x29.
    add     sp,  x29, #StubFrameContextSize
    ldr     x29, [x29]

    b       .Ltail_call_throw

    // =========================================================================
    // Tail-call ThrowException.
    // On entry:
    //   x0  = exception_obj
    //   x29 = restored frame pointer of the stub's caller
    //   x30 = return address in the stub's caller
    //   sp  = stack pointer of the stub's caller
    // =========================================================================
.Ltail_call_throw:
    adrp    x10, :got:g_ThrowExceptionPtr
    ldr     x10, [x10, :got_lo12:g_ThrowExceptionPtr]
    ldr     x10, [x10]
    br      x10                 // tail call; x30 carries the return address

.Lnot_stub:
    // Unwind one intermediate frame and continue the search.
    ldr     x29, [x29]
    b       .Lunwind_loop

.Lcrash:
    udf     #0                  // deliberately undefined; triggers SIGILL
