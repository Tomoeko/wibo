#include "macros.S"
#include "context_x64_offsets.h"
#include "software_exception_capture_offsets.h"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text

#ifdef __APPLE__
#define RAISE_CAPTURE_CONTINUATION LwiboRaiseCaptureContinuation
#else
#define RAISE_CAPTURE_CONTINUATION .LwiboRaiseCaptureContinuation
#endif

# Preserve the API caller before changing registers or entering host context.
.macro capture_software_context
	pushfq
	push rax
	push r11
	lea rsp, [rsp-WIBO_SOFTWARE_FRAME_SIZE]
	.cfi_def_cfa_offset (WIBO_SOFTWARE_FRAME_SIZE+32)
	lea r11, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	mov qword ptr [r11+WIBO_CTX64_RCX], rcx
	mov qword ptr [r11+WIBO_CTX64_RDX], rdx
	mov qword ptr [r11+WIBO_CTX64_RBX], rbx
	mov qword ptr [r11+WIBO_CTX64_RBP], rbp
	mov qword ptr [r11+WIBO_CTX64_RSI], rsi
	mov qword ptr [r11+WIBO_CTX64_RDI], rdi
	mov qword ptr [r11+WIBO_CTX64_R8], r8
	mov qword ptr [r11+WIBO_CTX64_R9], r9
	mov qword ptr [r11+WIBO_CTX64_R10], r10
	mov qword ptr [r11+WIBO_CTX64_R12], r12
	mov qword ptr [r11+WIBO_CTX64_R13], r13
	mov qword ptr [r11+WIBO_CTX64_R14], r14
	mov qword ptr [r11+WIBO_CTX64_R15], r15
	mov rax, qword ptr [rsp+WIBO_SOFTWARE_FRAME_SIZE+8]
	mov qword ptr [r11+WIBO_CTX64_RAX], rax
	mov rax, qword ptr [rsp+WIBO_SOFTWARE_FRAME_SIZE]
	mov qword ptr [r11+WIBO_CTX64_R11], rax
	lea rax, [rsp+WIBO_SOFTWARE_FRAME_SIZE+32]
	mov qword ptr [r11+WIBO_CTX64_RSP], rax
	mov rax, qword ptr [rsp+WIBO_SOFTWARE_FRAME_SIZE+24]
	mov qword ptr [r11+WIBO_CTX64_RIP], rax
	mov eax, dword ptr [rsp+WIBO_SOFTWARE_FRAME_SIZE+16]
	mov dword ptr [r11+WIBO_CTX64_EFLAGS], eax
	mov dword ptr [r11+WIBO_CTX64_FLAGS], WIBO_CTX64_CAPTURE_FLAGS
	stmxcsr [r11+WIBO_CTX64_MXCSR]
	mov word ptr [r11+WIBO_CTX64_CS], cs
	mov word ptr [r11+WIBO_CTX64_DS], ds
	mov word ptr [r11+WIBO_CTX64_ES], es
	mov word ptr [r11+WIBO_CTX64_FS], fs
	mov word ptr [r11+WIBO_CTX64_GS], gs
	mov word ptr [r11+WIBO_CTX64_SS], ss
	fxsave [r11+WIBO_CTX64_FXSAVE]
#ifdef __APPLE__
	call SYMBOL_NAME(enterHostContext)
#else
	xor eax, eax
#endif
	mov qword ptr [rsp+WIBO_SOFTWARE_FRAME_TEB], rax
.endm

.macro reenter_guest
#ifdef __APPLE__
	mov rdi, qword ptr [rsp+WIBO_SOFTWARE_FRAME_TEB]
	call SYMBOL_NAME(enterGuestContext)
#endif
.endm

.macro restore_software_registers
	lea r11, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	fxrstor [r11+WIBO_CTX64_FXSAVE]
	mov rbx, qword ptr [r11+WIBO_CTX64_RBX]
	mov rbp, qword ptr [r11+WIBO_CTX64_RBP]
	mov rsi, qword ptr [r11+WIBO_CTX64_RSI]
	mov rdi, qword ptr [r11+WIBO_CTX64_RDI]
	mov r12, qword ptr [r11+WIBO_CTX64_R12]
	mov r13, qword ptr [r11+WIBO_CTX64_R13]
	mov r14, qword ptr [r11+WIBO_CTX64_R14]
	mov r15, qword ptr [r11+WIBO_CTX64_R15]
.endm

.macro return_software_capture
	reenter_guest
	restore_software_registers
	mov rcx, qword ptr [r11+WIBO_CTX64_RCX]
	mov rdx, qword ptr [r11+WIBO_CTX64_RDX]
	mov r8, qword ptr [r11+WIBO_CTX64_R8]
	mov r9, qword ptr [r11+WIBO_CTX64_R9]
	mov r10, qword ptr [r11+WIBO_CTX64_R10]
	lea rsp, [rsp+WIBO_SOFTWARE_FRAME_SIZE]
	.cfi_def_cfa_offset 32
	pop r11
	pop rax
	popfq
	.cfi_def_cfa_offset 8
	ret
.endm

ASM_GLOBAL(wiboCaptureRaiseException64, @function)
	.cfi_startproc
	capture_software_context
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	mov esi, 1
	call SYMBOL_NAME(wiboPrepareSoftwareExceptionCapture64)
	reenter_guest
	restore_software_registers
	lea rcx, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE+WIBO_SOFTWARE_CAPTURE_LOCAL_RECORD]
	call SYMBOL_NAME(wiboCaptureRtlRaiseException64)
RAISE_CAPTURE_CONTINUATION:
	# The inner entry returns with guest context already active.
	restore_software_registers
	mov rcx, qword ptr [r11+WIBO_CTX64_RCX]
	mov rdx, qword ptr [r11+WIBO_CTX64_RDX]
	mov r8, qword ptr [r11+WIBO_CTX64_R8]
	mov r9, qword ptr [r11+WIBO_CTX64_R9]
	mov r10, qword ptr [r11+WIBO_CTX64_R10]
	lea rsp, [rsp+WIBO_SOFTWARE_FRAME_SIZE]
	.cfi_def_cfa_offset 32
	pop r11
	pop rax
	popfq
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
ASM_END(wiboCaptureRaiseException64)
	.globl SYMBOL_NAME(wiboRaiseCaptureContinuation64)
	.set SYMBOL_NAME(wiboRaiseCaptureContinuation64), RAISE_CAPTURE_CONTINUATION

ASM_GLOBAL(wiboCaptureRtlRaiseException64, @function)
	.cfi_startproc
	capture_software_context
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	xor esi, esi
	call SYMBOL_NAME(wiboPrepareSoftwareExceptionCapture64)
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	call SYMBOL_NAME(wiboConsumeSoftwareExceptionCapture64)
	return_software_capture
	.cfi_endproc
ASM_END(wiboCaptureRtlRaiseException64)

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
