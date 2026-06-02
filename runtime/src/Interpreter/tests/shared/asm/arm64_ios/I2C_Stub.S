    .arch armv8-a

    .text
    .align 4

    .global _I2C_Stub
_I2C_Stub:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    mov     x28, x5
    blr     x4

    ldp     x29, x30, [sp], #16
    ret
