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

#include "software_exception_frame_x64.inc"

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
	capture_software_context WIBO_SOFTWARE_FRAME_SIZE
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
	capture_software_context WIBO_SOFTWARE_FRAME_SIZE
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
