#include "../dll/context_x64_offsets.h"

#ifdef __APPLE__
#define LABEL_(name) _##name
#define LABEL(name) LABEL_(name)
#else
#define LABEL(name) name
#endif

.intel_syntax noprefix
.text

.macro seed_dispatch_state
	fninit
	mov word ptr [rsp+0xd8], 0x077f
	fldcw word ptr [rsp+0xd8]
	mov dword ptr [rsp+0xdc], 0x3f80
	ldmxcsr dword ptr [rsp+0xdc]
	fld1
	fldz
	mov eax, 0x26262626
	movd xmm6, eax
	pshufd xmm6, xmm6, 0
	mov eax, 0x3f3f3f3f
	movd xmm15, eax
	pshufd xmm15, xmm15, 0
	mov rbx, 0x2132435465768798
	mov rbp, 0x31425364758697a8
	mov rsi, 0x415263748596a7b8
	mov rdi, 0x5162738495a6b7c8
	mov r12, 0xa1b2c3d4e5f60718
	mov r13, 0xb1c2d3e4f5061728
	mov r14, 0xc1d2e3f405162738
	mov r15, 0xd1e2f30415263748
.endm

.macro snapshot_dispatch_state index
	# Save all selected GPRs and flags before using scratch registers or doing
	# arithmetic. The native caller state remains in the probe's outer frame.
	pushfq
	push rax
	push rcx
	push rdx
	push rbx
	push rbp
	push rsi
	push rdi
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	mov rax, qword ptr [rsp+0x80+0x38]
	lea rax, [rax+\index*1232]
	.macro snapshot_dispatch_gpr field, slot
		mov rdx, qword ptr [rsp+\slot]
		mov qword ptr [rax+\field], rdx
	.endm
	snapshot_dispatch_gpr WIBO_CTX64_RAX, 112
	snapshot_dispatch_gpr WIBO_CTX64_RCX, 104
	snapshot_dispatch_gpr WIBO_CTX64_RDX, 96
	snapshot_dispatch_gpr WIBO_CTX64_RBX, 88
	snapshot_dispatch_gpr WIBO_CTX64_RBP, 80
	snapshot_dispatch_gpr WIBO_CTX64_RSI, 72
	snapshot_dispatch_gpr WIBO_CTX64_RDI, 64
	snapshot_dispatch_gpr WIBO_CTX64_R8, 56
	snapshot_dispatch_gpr WIBO_CTX64_R9, 48
	snapshot_dispatch_gpr WIBO_CTX64_R10, 40
	snapshot_dispatch_gpr WIBO_CTX64_R11, 32
	snapshot_dispatch_gpr WIBO_CTX64_R12, 24
	snapshot_dispatch_gpr WIBO_CTX64_R13, 16
	snapshot_dispatch_gpr WIBO_CTX64_R14, 8
	snapshot_dispatch_gpr WIBO_CTX64_R15, 0
	.purgem snapshot_dispatch_gpr
	lea rdx, [rsp+0x80]
	mov qword ptr [rax+WIBO_CTX64_RSP], rdx
	mov rdx, qword ptr [rsp+0x80+0x30]
	mov rcx, qword ptr [rdx+\index*16]
	mov qword ptr [rax+WIBO_CTX64_RIP], rcx
	mov edx, dword ptr [rsp+120]
	mov dword ptr [rax+WIBO_CTX64_EFLAGS], edx
	stmxcsr [rax+WIBO_CTX64_MXCSR]
	fxsave [rax+WIBO_CTX64_FXSAVE]
	lea rsp, [rsp+0x80]
.endm

.globl LABEL(wiboSoftwareDispatchProbe64)
#ifdef __ELF__
.type LABEL(wiboSoftwareDispatchProbe64), @function
#endif
LABEL(wiboSoftwareDispatchProbe64):
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	# Shadow area, pointers, record, arguments, and original legacy FP state.
	sub rsp, 0x368
	mov qword ptr [rsp+0x20], rcx
	mov qword ptr [rsp+0x28], rdx
	mov qword ptr [rsp+0x30], r8
	mov qword ptr [rsp+0x38], r9
	fxsave [rsp+0x160]
	call LABEL(wiboEnterSoftwareDispatchFixtureGuest64)
	lea rdi, [rsp+0xe0]
	mov eax, 0x9000
	mov ecx, 15
argument_loop:
	mov qword ptr [rdi], rax
	lea rdi, [rdi+8]
	inc eax
	loop argument_loop
	mov rax, qword ptr [rsp+0x30]
	lea rdx, [rip+raise_return]
	mov qword ptr [rax], rdx
	mov qword ptr [rax+8], rsp
	seed_dispatch_state
	mov ecx, 0xe0420801
	mov edx, 0x80
	mov r8d, 16
	lea r9, [rsp+0xe0]
	stc
	call qword ptr [rsp+0x20]
raise_return:
	snapshot_dispatch_state 0
	mov rax, qword ptr [rsp+0x30]
	mov rdx, qword ptr [rsp+0xe0]
	mov qword ptr [rax+40], rdx
	lea rdi, [rsp+0x40]
	xor eax, eax
	mov ecx, 19
	cld
	rep stosq
	lea rcx, [rsp+0x40]
	mov dword ptr [rcx], 0xe0420801
	mov qword ptr [rcx+16], 0x1234
	mov dword ptr [rcx+24], 1
	mov qword ptr [rcx+32], 0x4567
	mov rax, qword ptr [rsp+0x30]
	lea rdx, [rip+rtl_return]
	mov qword ptr [rax+16], rdx
	mov qword ptr [rax+24], rsp
	mov qword ptr [rax+32], rcx
	seed_dispatch_state
	lea rcx, [rsp+0x40]
	stc
	call qword ptr [rsp+0x28]
rtl_return:
	snapshot_dispatch_state 1
	mov rax, qword ptr [rsp+0x30]
	mov rdx, qword ptr [rsp+0x40+32]
	mov qword ptr [rax+48], rdx
	# Restore the probe's native nonvolatile and FP state before C++ resumes.
	fxrstor [rsp+0x160]
	call LABEL(wiboLeaveSoftwareDispatchFixtureGuest64)
	# The native helper may clobber Windows-nonvolatile SSE state.
	fxrstor [rsp+0x160]
	add rsp, 0x368
	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	ret
#ifdef __ELF__
.size LABEL(wiboSoftwareDispatchProbe64), .-LABEL(wiboSoftwareDispatchProbe64)
.section .note.GNU-stack, "", @progbits
#endif
