    .arch armv8-a

    .text
    .align 4

    .global _I2I_Stub
_I2I_Stub:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    blr     x4

    ldp     x29, x30, [sp], #16
    ret

    .global _I2I_Stub_End
_I2I_Stub_End:
    nop
