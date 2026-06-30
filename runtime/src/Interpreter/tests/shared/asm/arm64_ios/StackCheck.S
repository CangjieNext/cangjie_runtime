.arch armv8-a

    .text
    .align 4

.global _StackCheck
    .extern _g_CJ_MCC_StackGrowStubPtr
    .extern _g_InterpreterStackReserveSize
_StackCheck:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    str     x28, [sp, #-16]!

    mov     x28, x0
    ldr     x9, [x28, #40]

    adrp    x11, _g_InterpreterStackReserveSize@GOTPAGE
    ldr     x11, [x11, _g_InterpreterStackReserveSize@GOTPAGEOFF]
    ldr     x11, [x11]

    add     x9, x9, x11

    mov     x12, sp
    cmp     x12, x9
    bls     .Lstack_overflow    // Jump if Below or Equal (Unsigned comparison)

.Lstack_check_end:

    ldr     x28, [sp], #16
    ldp     x29, x30, [sp], #16
    ret

.Lstack_overflow:
    // Full frame kept: x29 is still C2I's frame pointer.
    // In case of SOE, with the frame in the x29 chain, BuildEHFrameInfo sees C2I and makes it
    adrp    x9, _g_InterpreterStackReserveSize@GOTPAGE
    ldr     x9, [x9, _g_InterpreterStackReserveSize@GOTPAGEOFF]
    ldr     x9, [x9]

    adrp    x10, _g_CJ_MCC_StackGrowStubPtr@GOTPAGE
    ldr     x10, [x10, _g_CJ_MCC_StackGrowStubPtr@GOTPAGEOFF]
    ldr     x10, [x10]
    blr     x10
    // No return in case of SOE
    // After StackGrowStub returns: rbp = new_frame.rbp, rsp = new_frame.rbp - SavedRegs.

    b       .Lstack_check_end
