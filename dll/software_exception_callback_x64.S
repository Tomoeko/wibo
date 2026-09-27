#include "macros.S"
#include "software_exception_activation_offsets.h"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text

.macro save_software_callback_context
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
.endm

.macro restore_software_callback_context
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
.endm

ASM_GLOBAL(wiboCallVectoredHandler64, @function)
	.cfi_startproc
	save_software_callback_context
	mov rax, rdi
	mov rcx, rsi
	call rax
	restore_software_callback_context
	.cfi_endproc
ASM_END(wiboCallVectoredHandler64)

ASM_GLOBAL(wiboCallFrameHandler64, @function)
	.cfi_startproc
	save_software_callback_context
#ifdef __APPLE__
	lea r10, [rip+LwiboFrameHandlerContinuation]
#else
	lea r10, [rip+.LwiboFrameHandlerContinuation]
#endif
	mov qword ptr [r9+WIBO_SOFTWARE_ACTIVATION_BRIDGE_RIP], r10
	mov qword ptr [r9+WIBO_SOFTWARE_ACTIVATION_BRIDGE_RSP], rsp
	mov qword ptr [r9+WIBO_SOFTWARE_ACTIVATION_STACK_LOW], rsp
	mov rax, rdi
	mov r9, r8
	mov r8, rcx
	mov rcx, rsi
	call rax
#ifdef __APPLE__
LwiboFrameHandlerContinuation:
#else
.LwiboFrameHandlerContinuation:
#endif
	restore_software_callback_context
	.cfi_endproc
ASM_END(wiboCallFrameHandler64)
	.globl SYMBOL_NAME(wiboFrameHandlerContinuation64)
#ifdef __APPLE__
	.set SYMBOL_NAME(wiboFrameHandlerContinuation64), LwiboFrameHandlerContinuation
#else
	.set SYMBOL_NAME(wiboFrameHandlerContinuation64), .LwiboFrameHandlerContinuation
#endif
#endif
