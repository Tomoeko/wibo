.intel_syntax noprefix
.text
.p2align 4
.globl consolidation_outer
.seh_proc consolidation_outer
.seh_handler consolidation_outer_handler, @except, @unwind
consolidation_outer:
    push rbx
    .seh_pushreg rbx
    push r12
    .seh_pushreg r12
    sub rsp, 88
    .seh_stackalloc 88
    movdqa xmmword ptr [rsp+48], xmm6
    .seh_savexmm xmm6, 48
    .seh_endprologue
    mov qword ptr [rip+outer_rsp], rsp
    mov rax, qword ptr [rip+expected_cookie]
    mov qword ptr [rsp+32], rax
    mov rbx, 0x1122334455667788
    mov r12, 0x33445566778899aa
    movdqa xmm6, xmmword ptr [rip+expected_xmm6]
    cmp dword ptr [rip+direct_mode], 0
    je .Ltarget_call
    lea rcx, [rip+selected_context]
    call qword ptr [rip+capture_entry]
    lea rax, [rip+consolidation_decoy]
    mov qword ptr [rip+selected_context+248], rax
    mov rax, 0x123456789abcdef0
    mov qword ptr [rip+selected_context+120], rax
.Ltarget_call:
    call consolidation_inner
.globl consolidation_after_inner
consolidation_after_inner:
    nop
    mov dword ptr [rip+unexpected_return], 1
    jmp .Lcomplete
.globl consolidation_decoy
consolidation_decoy:
    mov dword ptr [rip+unexpected_return], 2
    jmp .Lcomplete
.globl consolidation_landing
consolidation_landing:
    mov qword ptr [rip+landed_rsp], rsp
    mov qword ptr [rip+landed_rax], rax
    mov qword ptr [rip+landed_rbx], rbx
    mov qword ptr [rip+landed_r12], r12
    movdqu xmmword ptr [rip+landed_xmm6], xmm6
    mov dword ptr [rip+landed], 1
    imul eax, dword ptr [rip+trace], 10
    add eax, 5
    mov dword ptr [rip+trace], eax
.Lcomplete:
    movdqa xmm6, xmmword ptr [rsp+48]
    add rsp, 88
    pop r12
    pop rbx
    ret
.seh_handlerdata
    .long 0x31415926
.text
.seh_endproc

.p2align 4
.globl consolidation_inner
.seh_proc consolidation_inner
.seh_handler consolidation_inner_handler, @except, @unwind
consolidation_inner:
    push r12
    .seh_pushreg r12
    sub rsp, 80
    .seh_stackalloc 80
    movdqa xmmword ptr [rsp+48], xmm6
    .seh_savexmm xmm6, 48
    .seh_endprologue
    mov qword ptr [rip+inner_rsp], rsp
    mov r12, 0x2233445566778899
    pxor xmm6, xmm6
    cmp dword ptr [rip+direct_mode], 0
    je .Lunwind
    lea rcx, [rip+selected_context]
    lea rdx, [rip+consolidation_record]
    call qword ptr [rip+restore_entry]
    nop
    jmp .Lreturned
.Lunwind:
    lea rax, [rip+selected_context]
    mov qword ptr [rsp+32], rax
    mov qword ptr [rsp+40], 0
    mov rcx, qword ptr [rip+outer_rsp]
    lea rdx, [rip+consolidation_decoy]
    lea r8, [rip+consolidation_record]
    mov r9, 0x123456789abcdef0
    call qword ptr [rip+unwind_entry]
    nop
.Lreturned:
    mov dword ptr [rip+unexpected_return], 3
    movdqa xmm6, xmmword ptr [rsp+48]
    add rsp, 80
    pop r12
    ret
.seh_handlerdata
    .long 0x27182818
.text
.seh_endproc

.p2align 4
.globl consolidation_callback
.seh_proc consolidation_callback
consolidation_callback:
    sub rsp, 40
    .seh_stackalloc 40
    .seh_endprologue
    mov qword ptr [rip+callback_rsp], rsp
    mov qword ptr [rip+callback_rbx], rbx
    mov qword ptr [rip+callback_r12], r12
    movdqu xmmword ptr [rip+callback_xmm6], xmm6
    call consolidation_callback_body
    nop
    add rsp, 40
    ret
.seh_endproc
