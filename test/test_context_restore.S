#include "../dll/context_x64_offsets.h"

#define RESTORE_PROBE_FRAME_SIZE 0x260

.intel_syntax noprefix
.text
.globl wiboContextRestoreProbe
.seh_proc wiboContextRestoreProbe
wiboContextRestoreProbe:
	push rbx
	.seh_pushreg rbx
	# Reserve shadow space, four pointer slots, and the original FP state.
	sub rsp, RESTORE_PROBE_FRAME_SIZE
	.seh_stackalloc RESTORE_PROBE_FRAME_SIZE
	.seh_endprologue
	mov qword ptr [rsp+0x20], rcx
	mov qword ptr [rsp+0x28], r8
	mov qword ptr [rsp+0x30], r9
	mov qword ptr [rsp+0x38], rdx
	fxsave [rsp+0x40]
	mov qword ptr [r9], rsp
	lea rax, [rip+.Lrestore_landing]
	mov qword ptr [r9+24], rax
	call qword ptr [rsp+0x38]
.Lcapture_return:
	mov rcx, qword ptr [rsp+0x20]
	mov rdx, qword ptr [rsp+0x30]
	mov rax, qword ptr [rcx+WIBO_CTX64_RSP]
	mov qword ptr [rdx+8], rax
	mov rax, qword ptr [rcx+WIBO_CTX64_RIP]
	mov qword ptr [rdx+16], rax
	mov eax, dword ptr [rcx+WIBO_CTX64_FLAGS]
	mov dword ptr [rdx+80], eax
	cmp eax, WIBO_CTX64_CAPTURE_FLAGS
	jne .Lunsupported_capture
	cmp qword ptr [rcx+WIBO_CTX64_RSP], rsp
	jne .Lunsupported_capture
	lea rax, [rip+.Lcapture_return]
	cmp qword ptr [rcx+WIBO_CTX64_RIP], rax
	jne .Lunsupported_capture
	# Do not restore an inherited state with unmasked FP exceptions.
	movzx eax, word ptr [rcx+WIBO_CTX64_FXSAVE]
	cmp ax, word ptr [rsp+0x40]
	jne .Lunsupported_capture
	and eax, 0x3f
	cmp eax, 0x3f
	jne .Lunsupported_capture
	mov eax, dword ptr [rcx+WIBO_CTX64_FXSAVE+24]
	cmp eax, dword ptr [rsp+0x40+24]
	jne .Lunsupported_capture
	and eax, 0x1f80
	cmp eax, 0x1f80
	jne .Lunsupported_capture
	# Keep the captured stack; only the continuation and selected values change.
	lea rax, [rip+.Lrestore_landing]
	mov qword ptr [rcx+WIBO_CTX64_RIP], rax
	mov rax, 0x123456789abcdef0
	mov qword ptr [rcx+WIBO_CTX64_RAX], rax
	mov rax, 0x0fedcba987654321
	mov qword ptr [rcx+WIBO_CTX64_RBX], rax
	mov rax, 0x1020304050607080
	mov qword ptr [rcx+WIBO_CTX64_FXSAVE+160+6*16], rax
	mov rax, 0x90a0b0c0d0e0f000
	mov qword ptr [rcx+WIBO_CTX64_FXSAVE+160+6*16+8], rax
	xor eax, eax
	xor ebx, ebx
	pxor xmm6, xmm6
	xor edx, edx
	call qword ptr [rsp+0x28]
	# Returning from restoration is a normal fixture failure, not a fault.
	mov rdx, qword ptr [rsp+0x30]
	mov dword ptr [rdx+72], 1
	jmp .Lprobe_complete
.Lrestore_landing:
	# Snapshot the restored values before cleanup changes any of them.
	mov rdx, qword ptr [rsp+0x30]
	mov qword ptr [rdx+32], rsp
	mov qword ptr [rdx+40], rax
	mov qword ptr [rdx+48], rbx
	movdqu xmmword ptr [rdx+56], xmm6
	stmxcsr [rdx+84]
	mov dword ptr [rdx+76], 1
	jmp .Lprobe_complete
.Lunsupported_capture:
	mov dword ptr [rdx+88], 1
.Lprobe_complete:
	# Preserve the native caller's nonvolatile integer and FP state.
	fxrstor [rsp+0x40]
	add rsp, RESTORE_PROBE_FRAME_SIZE
	pop rbx
	ret
.seh_endproc
