    .arch armv8-a

    .set VIRTUAL_FUNCS_N, 10

    // Frame layout mirrors CalleeSavedStub.S so that RecordStubCalleeSaved
    // (StackMapAarch64.h) can trace GC roots held in callee-saved registers:
    //
    //   StubFrameContextSize   = 8 * 14 = 112 bytes  (total frame allocation)
    //   StubCalleeSaveAreaSize = 8 *  4 =  32 bytes  (offset to first callee-saved slot)
    //
    //   [x29 +   0]: saved x29 (prev fp)
    //   [x29 +   8]: saved x30 (lr)
    //   [x29 +  16]: (padding - unused)
    //   [x29 +  24]: (padding - unused)
    //   [x29 +  32]: x19  ← calleeSavedAreaOffset  (RecordStubCalleeSaved starts here)
    //   [x29 +  40]: x20
    //   [x29 +  48]: x21
    //   [x29 +  56]: x22
    //   [x29 +  64]: x23
    //   [x29 +  72]: x24
    //   [x29 +  80]: x25
    //   [x29 +  88]: x26
    //   [x29 +  96]: x27
    //   [x29 + 104]: x28 (thread-local data)
    .set StubFrameContextSize,   (8 * 14)
    .set StubCalleeSaveAreaSize, (8 * 4)

    .extern virtualFuncs
    .extern g_getPendingExceptionPtr

    // Each trampoline is 16 bytes (4x4-byte AArch64 instructions):
    //   adrp  x16, :got:virtualFuncs
    //   ldr   x16, [x16, :got_lo12:virtualFuncs]
    //   ldr   x0,  [x16, #(idx*8)]      ; x0 = CALLEE_ADDR = virtualFuncs.data[idx-1]
    //   b     C2I_Stub
    //
    // The virtualFuncs struct has an 8-byte padding field first, so
    // data[0] lives at offset 8, data[k] at offset (k+1)*8.
    // idx runs 1..VIRTUAL_FUNCS_N → loads data[0]..data[VIRTUAL_FUNCS_N-1].
    .macro trampoline_entry idx
    adrp    x16, :got:virtualFuncs
    ldr     x16, [x16, :got_lo12:virtualFuncs]
    ldr     x0,  [x16, #(\idx * 8)]
    b       C2I_Stub
    .endm

    // Recursive macro that emits `count` trampolines starting at idx=1.
    .macro trampoline_table count, idx=1
    .if \idx <= \count
    trampoline_entry \idx
    trampoline_table \count, (\idx + 1)
    .endif
    .endm

    .text
    .align 4

    .global trampolinesStart
trampolinesStart:
    trampoline_table VIRTUAL_FUNCS_N


    .global C2I_Stub
    .extern g_GetThreadLocalDataPtr
    .align 4
C2I_Stub:
    // x0  = CALLEE_ADDR (interpreter function, set by trampoline).
    // x28 = thread-local data

    // Allocate frame.
    stp     x29, x30, [sp, #-StubFrameContextSize]!
    mov     x29, sp

    // Save x19-x28 corresponding to layout of RecordStubCalleeSaved.
    stp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    stp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    stp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    stp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    stp     x27, x28, [x29, #StubCalleeSaveAreaSize + 0x40]

    // Call interpreter function with TLD as the first argument.
    mov     x9,  x0             // stash CALLEE_ADDR before overwriting x0
    mov     x0,  x28            // arg0 = TLD
    blr     x9

    // Refresh x28 (TLD) after the interpreter returns.
    // TODO it is unnecessary if interpreter does TLD update by itself.
    //
    // Temporarily store the interpreter's return value (x0) in the saved-x28
    // slot so it survives the g_GetThreadLocalDataPtr call.
    str     x0,  [x29, #StubCalleeSaveAreaSize + 0x48]

    adrp    x10, :got:g_GetThreadLocalDataPtr
    ldr     x10, [x10, :got_lo12:g_GetThreadLocalDataPtr]
    ldr     x10, [x10]
    blr     x10                 // x0 = fresh TLD
    mov     x28, x0             // x28 = refreshed TLD

    // Restore callee-saved registers x19-x27; skip x28 (just refreshed above).
    ldp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    ldp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    ldp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    ldp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    ldr     x27,      [x29, #StubCalleeSaveAreaSize + 0x40]

    // Restore interpreter's return value.
    ldr     x0,  [x29, #StubCalleeSaveAreaSize + 0x48]

    ldp     x29, x30, [sp], #StubFrameContextSize
    ret

    .global C2I_Stub_End
C2I_Stub_End:
    nop
