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
    .extern g_CJ_MCC_StackGrowStubPtr
    .extern g_GetThreadLocalDataPtr
    .extern g_InterpreterStackReserveSize
    .align 4
C2I_Stub:
    // x0  = CALLEE_ADDR (interpreter function, set by trampoline).
    // x28 = thread-local data

    // Allocate frame.
    stp     x29, x30, [sp, #-StubFrameContextSize]!
    mov     x29, sp

    // Stack overflow check: ThreadLocalData::protectAddr is at offset 40 from TLD.
    ldr     x9,  [x28, #40]
    adrp    x10, :got:g_InterpreterStackReserveSize
    ldr     x10, [x10, :got_lo12:g_InterpreterStackReserveSize]
    ldr     x10, [x10]
    add     x9,  x9, x10        // x9 = stack_limit + reserve_size
    cmp     sp,  x9
    bls     .Lstack_overflow

.Lstack_check_end:
    // Save x19-x28 corresponding to layout of RecordStubCalleeSaved.
    stp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    stp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    stp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    stp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    stp     x27, x28, [x29, #StubCalleeSaveAreaSize + 0x40]

.Lcall_interpreter:
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

.Lstack_overflow:
    // Full frame kept: x29 is still C2I's frame pointer.
    // In case of SOE, with the frame in the x29 chain, BuildEHFrameInfo sees C2I and makes it
    // lastRuntimeFrame; ChangeEHStackInfoLR patches C2I's saved x30 ([x29+8])
    // to RestoreContextForEH. StackGrowStub is NOT patched and returns here.

    // Save x19-x28 so the callee-save area is initialised before StackGrowStub
    // calls RecordStackPtrs (which reads these slots via RecordStubCalleeSaved).
    // Without this, the area would contain garbage and corrupt regSlotsMap for
    // derived-pointer adjustment in managed frames below C2I.
    stp     x19, x20, [x29, #StubCalleeSaveAreaSize]
    stp     x21, x22, [x29, #StubCalleeSaveAreaSize + 0x10]
    stp     x23, x24, [x29, #StubCalleeSaveAreaSize + 0x20]
    stp     x25, x26, [x29, #StubCalleeSaveAreaSize + 0x30]
    stp     x27, x28, [x29, #StubCalleeSaveAreaSize + 0x40]

    // CJ_MCC_StackGrowStub (AArch64) reads the required size from its saved-x9
    // slot at [sp+0x58] in its own frame; load the value into x9 before the call.
    mov     x9,  #StubFrameContextSize
    adrp    x10, :got:g_InterpreterStackReserveSize
    ldr     x10, [x10, :got_lo12:g_InterpreterStackReserveSize]
    ldr     x10, [x10]
    add     x9,  x9,  x10

    // CJ_MCC_StackGrowStub saves and restores all registers (including x0 =
    // CALLEE_ADDR), so CALLEE_ADDR survives the call.
    adrp    x10, :got:g_CJ_MCC_StackGrowStubPtr
    ldr     x10, [x10, :got_lo12:g_CJ_MCC_StackGrowStubPtr]
    ldr     x10, [x10]
    blr     x10

    // Check for pending SOE (sp = C2I.x29 after StackGrowStub returns).
    str     x0,  [sp, #-16]!               // save CALLEE_ADDR
    adrp    x10, :got:g_getPendingExceptionPtr
    ldr     x10, [x10, :got_lo12:g_getPendingExceptionPtr]
    ldr     x10, [x10]
    blr     x10                            // x0 = pending exception or null
    mov     x11, x0                        // preserve pending exception result
    ldr     x0,  [sp], #16                 // restore CALLEE_ADDR; sp = C2I.x29
    cbnz    x11, .Lsoe_return

    // No SOE: callee-saves already written above; jump past the stp block.
    b       .Lcall_interpreter

.Lsoe_return:
    // C2I[x29+8] was patched to RestoreContextForEH by ChangeEHStackInfoLR.
    // Restoring the frame sets x29=caller.x29, x30=RestoreContextForEH, sp=original_sp.
    // x19-x28 are already the caller's values (overflow path never modified them).
    ldp     x29, x30, [sp], #StubFrameContextSize
    ret                                     // RestoreContextForEH

    .global C2I_Stub_End
C2I_Stub_End:
    nop
