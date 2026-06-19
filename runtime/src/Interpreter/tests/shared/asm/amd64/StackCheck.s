    cpu x64
    bits 64
    default rel

section .text  align=16

global StackCheck
    extern g_CJ_MCC_StackGrowStubPtr
    extern g_InterpreterStackReserveSize
StackCheck:
    %define SAVED_RSP_REG rbp

    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    push    r15
    push    r15 ; for alignment
    mov     r15, rdi

    ; Fetch the base stack limit
    mov     rax, [r15 + 40]
    
    ; Fetch the required interpreter reserve size
    mov     r11, [rel g_InterpreterStackReserveSize wrt ..got]
    mov     r11, [r11] 

    add     rax, r11 ; add the reserve size to the limit
    
    cmp     rsp, rax
    jbe     .Lstack_overflow

.Lstack_check_end:
    pop     r15
    pop     r15
    pop     SAVED_RSP_REG
    ret

.Lstack_overflow:
    ; Pass the total required size (Interpreter Reserve) in rax
    mov     rax, [rel g_InterpreterStackReserveSize wrt ..got]
    mov     rax, [rax]

    mov     r10, [rel g_CJ_MCC_StackGrowStubPtr wrt ..got]
    mov     r10, [r10]
    call    r10
    ; No return in case of SOE
    ; After StackGrowStub returns: rbp = new_frame.rbp, rsp = new_frame.rbp - SavedRegs.

    jmp     .Lstack_check_end
