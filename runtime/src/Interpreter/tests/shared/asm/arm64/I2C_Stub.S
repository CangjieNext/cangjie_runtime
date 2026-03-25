    .arch armv8-a

    .text
    .align 4

    // Signature:
    //   uintptr_t I2C_Stub(uintptr_t arg1,            // x0
    //                      uintptr_t arg2,            // x1
    //                      uintptr_t arg3,            // x2
    //                      uintptr_t arg4,            // x3
    //                      void*     funcAddr,        // x4
    //                      DYN_ThreadLocalData tld);     // x5
    //
    // Sets x28 (EE) from the threadLocalData argument, then calls funcAddr
    // with arg1–arg4 in x0–x3 (unchanged).

    .global I2C_Stub
I2C_Stub:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    mov     x28, x5     // x28 = thread-local data
    blr     x4          // call funcAddr

    ldp     x29, x30, [sp], #16
    ret
