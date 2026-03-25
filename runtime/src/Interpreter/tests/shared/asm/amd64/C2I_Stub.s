    cpu x64
    bits 64
    default rel

section .data  align=16

    extern virtualFuncs ; defined in libinterpreter.cpp

section .text  align=16

%define VIRTUAL_FUNCS_N 10
%define CALLEE_ADDR rdi

    global  trampolinesStart
    align 16
trampolinesStart:
    %assign i 1
    %rep VIRTUAL_FUNCS_N
        mov rax, [rel virtualFuncs wrt ..got]
        mov CALLEE_ADDR, [rax + i*8]
        jmp near C2I_Stub
        %assign i i + 1
    %endrep


global C2I_Stub
    extern g_CJ_MCC_StackGrowStubPtr
    extern g_GetThreadLocalDataPtr
    extern g_InterpreterStackReserveSize
    extern g_getPendingExceptionPtr
    
C2I_Stub:
    %define SAVED_RSP_REG rbp
    %define CalleeSavedRegistersStubFrameSize 8 * 8
    
    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    sub     rsp, CalleeSavedRegistersStubFrameSize

    ; Fetch the base stack limit
    mov     rax, [r15 + 40]
    
    ; Fetch the required interpreter reserve size
    mov     r11, [rel g_InterpreterStackReserveSize wrt ..got]
    mov     r11, [r11] 

    add     rax, r11 ; add the reserve size to the limit
    
    cmp     rsp, rax
    jbe     .Lstack_overflow
.Lstack_check_end:
    ; =========================================================================

    ; save non-volatile registers allowing GC-tracing of contained references
    mov     [SAVED_RSP_REG - 8], r15 ; save old EE, expected by cjnative-GC
    mov     [SAVED_RSP_REG - 16], r14
    mov     [SAVED_RSP_REG - 24], r13
    mov     [SAVED_RSP_REG - 32], r12
    mov     [SAVED_RSP_REG - 40], rbx

.Lcall_interpreter:
    mov     rax, CALLEE_ADDR
    mov     rdi, r15 ; pass EE as first arg
    call    rax

    ; TODO rethrow possible exception

    ; Refresh r15 (EE) after the interpreter returns: the TLD pointer may have
    ; changed (e.g. after a GC safepoint).  Temporarily save the interpreter's
    ; return value (rax) in the saved-r15 slot so it survives the call.
    mov     [SAVED_RSP_REG - 8], rax

    mov     r10, [rel g_GetThreadLocalDataPtr wrt ..got]
    mov     r10, [r10]
    call    r10
    mov     r15, rax            ; r15 = refreshed EE

    ; restore other saved registers
    mov     r14, [SAVED_RSP_REG - 16]
    mov     r13, [SAVED_RSP_REG - 24]
    mov     r12, [SAVED_RSP_REG - 32]
    mov     rbx, [SAVED_RSP_REG - 40]

    ; restore interpreter's return value
    mov     rax, [SAVED_RSP_REG - 8]
    add     rsp, CalleeSavedRegistersStubFrameSize

    pop     SAVED_RSP_REG
    ret

.Lstack_overflow:
    ; Save callee-saved registers so RecordStubCalleeSaved() (called inside StackGrowStub via RecordStackPtrs) then
    ; finds the correct register values and builds an accurate regSlotsMap.
    mov     [SAVED_RSP_REG - 8], r15
    mov     [SAVED_RSP_REG - 16], r14
    mov     [SAVED_RSP_REG - 24], r13
    mov     [SAVED_RSP_REG - 32], r12
    mov     [SAVED_RSP_REG - 40], rbx

    ; Pass the total required size (Stub Frame + Interpreter Reserve) in rax
    mov     rax, CalleeSavedRegistersStubFrameSize
    mov     r11, [rel g_InterpreterStackReserveSize wrt ..got]
    mov     r11, [r11]
    add     rax, r11

    mov     r10, [rel g_CJ_MCC_StackGrowStubPtr wrt ..got]
    mov     r10, [r10]
    call    r10                             ; StackGrow; C2I is lastRuntimeFrame so only
                                            ; C2I[rbp+8] was patched, not StackGrowStub
    ; After StackGrowStub returns: rbp = new_C2I.rbp, rsp = new_C2I.rbp - CalleeSavedRegistersStubFrameSize.

    ; Check for pending SOE.
    push    rdi                             ; save CALLEE_ADDR
    mov     r10, [rel g_getPendingExceptionPtr wrt ..got]
    mov     r10, [r10]
    call    r10                             ; rax = pending exception or null
    pop     rdi                             ; restore CALLEE_ADDR
    test    rax, rax
    jnz     .Lsoe_return

    ; No SOE: callee-saves already written above;
    jmp     .Lcall_interpreter

.Lsoe_return:
    ; C2I[rbp+8] was patched to RestoreContextForEH by ChangeEHStackInfoLR.
    ; rsp is currently rbp - CalleeSavedRegistersStubFrameSize; advance to rbp so that
    ; pop restores caller's rbp and ret jumps to RestoreContextForEH.
    mov     rsp, rbp                        ; rsp = rbp ([rbp]=caller.rbp, [rbp+8]=patched RA)
    pop     SAVED_RSP_REG                   ; restore caller rbp; rsp = rbp+8
    ret                                     ; RA should be patched to RestoreContextForEH

    global C2I_Stub_End
C2I_Stub_End:
    nop