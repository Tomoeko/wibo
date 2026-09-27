#include "macros.S"
#include "context_x64_offsets.h"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text

# void ms_abi wiboCaptureContext64(CONTEXT64 *context)
ASM_GLOBAL(wiboCaptureContext64, @function)
	.cfi_startproc
	pushfq
	.cfi_adjust_cfa_offset 8
	mov qword ptr [rcx+WIBO_CTX64_RAX], rax
	mov qword ptr [rcx+WIBO_CTX64_RCX], rcx
	mov qword ptr [rcx+WIBO_CTX64_RDX], rdx
	mov qword ptr [rcx+WIBO_CTX64_RBX], rbx
	mov qword ptr [rcx+WIBO_CTX64_RBP], rbp
	mov qword ptr [rcx+WIBO_CTX64_RSI], rsi
	mov qword ptr [rcx+WIBO_CTX64_RDI], rdi
	mov qword ptr [rcx+WIBO_CTX64_R8], r8
	mov qword ptr [rcx+WIBO_CTX64_R9], r9
	mov qword ptr [rcx+WIBO_CTX64_R10], r10
	mov qword ptr [rcx+WIBO_CTX64_R11], r11
	mov qword ptr [rcx+WIBO_CTX64_R12], r12
	mov qword ptr [rcx+WIBO_CTX64_R13], r13
	mov qword ptr [rcx+WIBO_CTX64_R14], r14
	mov qword ptr [rcx+WIBO_CTX64_R15], r15
	lea rax, [rsp+16]
	mov qword ptr [rcx+WIBO_CTX64_RSP], rax
	mov rax, qword ptr [rsp+8]
	mov qword ptr [rcx+WIBO_CTX64_RIP], rax
	mov dword ptr [rcx+WIBO_CTX64_FLAGS], WIBO_CTX64_CAPTURE_FLAGS
	stmxcsr [rcx+WIBO_CTX64_MXCSR]
	mov word ptr [rcx+WIBO_CTX64_CS], cs
	mov word ptr [rcx+WIBO_CTX64_DS], ds
	mov word ptr [rcx+WIBO_CTX64_ES], es
	mov word ptr [rcx+WIBO_CTX64_FS], fs
	mov word ptr [rcx+WIBO_CTX64_GS], gs
	mov word ptr [rcx+WIBO_CTX64_SS], ss
	fxsave [rcx+WIBO_CTX64_FXSAVE]
	pop rax
	.cfi_adjust_cfa_offset -8
	mov dword ptr [rcx+WIBO_CTX64_EFLAGS], eax
	ret
	.cfi_endproc
ASM_END(wiboCaptureContext64)

# noreturn void ms_abi wiboRestoreContext64(const CONTEXT64 *context)
ASM_GLOBAL(wiboRestoreContext64, @function)
	.cfi_startproc
	mov r11, rcx
	fxrstor [r11+WIBO_CTX64_FXSAVE]
	mov eax, dword ptr [r11+WIBO_CTX64_EFLAGS]
	push rax
	.cfi_adjust_cfa_offset 8
	popfq
	.cfi_adjust_cfa_offset -8
	.cfi_undefined rip
	mov rsp, qword ptr [r11+WIBO_CTX64_RSP]
	# The return slot transfers control without reserving a scratch register.
	push qword ptr [r11+WIBO_CTX64_RIP]
	mov rax, qword ptr [r11+WIBO_CTX64_RAX]
	mov rcx, qword ptr [r11+WIBO_CTX64_RCX]
	mov rdx, qword ptr [r11+WIBO_CTX64_RDX]
	mov rbx, qword ptr [r11+WIBO_CTX64_RBX]
	mov rbp, qword ptr [r11+WIBO_CTX64_RBP]
	mov rsi, qword ptr [r11+WIBO_CTX64_RSI]
	mov rdi, qword ptr [r11+WIBO_CTX64_RDI]
	mov r8, qword ptr [r11+WIBO_CTX64_R8]
	mov r9, qword ptr [r11+WIBO_CTX64_R9]
	mov r10, qword ptr [r11+WIBO_CTX64_R10]
	mov r12, qword ptr [r11+WIBO_CTX64_R12]
	mov r13, qword ptr [r11+WIBO_CTX64_R13]
	mov r14, qword ptr [r11+WIBO_CTX64_R14]
	mov r15, qword ptr [r11+WIBO_CTX64_R15]
	mov r11, qword ptr [r11+WIBO_CTX64_R11]
	ret
	.cfi_endproc
ASM_END(wiboRestoreContext64)
#endif
