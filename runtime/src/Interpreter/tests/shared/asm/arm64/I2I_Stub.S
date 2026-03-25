    .arch armv8-a

    .text
    .align 4

    // Signature:
    //   void I2I_Stub(uintptr_t arg1,   // x0
    //                 uintptr_t arg2,   // x1
    //                 uintptr_t arg3,   // x2
    //                 uintptr_t arg4,   // x3
    //                 void*     funcAddr); // x4
    //
    // Interpreter-to-interpreter stub: calls funcAddr with arg1–arg4 in x0–x3.

    .global I2I_Stub
I2I_Stub:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    blr     x4          // call funcAddr

    ldp     x29, x30, [sp], #16
    ret

    .global I2I_Stub_End
I2I_Stub_End:
    nop
