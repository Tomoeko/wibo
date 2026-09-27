#include "macros.S"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text

ASM_GLOBAL(wiboCallVectoredHandler64, @function)
	.cfi_startproc
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	.cfi_def_cfa_offset 72
	sub rsp, 0x228
	.cfi_def_cfa_offset 624
	fxsave [rsp+0x20]
	mov rax, rdi
	mov rcx, rsi
	call rax
	fxrstor [rsp+0x20]
	add rsp, 0x228
	.cfi_def_cfa_offset 72
	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
ASM_END(wiboCallVectoredHandler64)
#endif
