#include "../dll/context_x64_offsets.h"

#ifdef __APPLE__
#define LABEL_(name) _##name
#define LABEL(name) LABEL_(name)
#else
#define LABEL(name) name
#endif

#define PROBE_FRAME_SIZE 0x258
#define PROBE_FIFTH_ARGUMENT (PROBE_FRAME_SIZE + 8*8 + 5*8)

.intel_syntax noprefix
.text
.globl LABEL(wiboContextProbe)
#ifdef __ELF__
.type LABEL(wiboContextProbe), @function
#endif
LABEL(wiboContextProbe):
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	# Reserve the call area, locals, and original 512-byte floating-point state.
	sub rsp, PROBE_FRAME_SIZE
	mov qword ptr [rsp+0x20], rcx
	mov qword ptr [rsp+0x28], rdx
	mov qword ptr [rsp+0x30], r8
	mov qword ptr [rsp+0x38], r9
	mov dword ptr [rsp+0x40], 0
	mov eax, dword ptr [rsp+PROBE_FIFTH_ARGUMENT]
	mov dword ptr [rsp+0x44], eax
	fxsave [rsp+0x50]
	mov qword ptr [r8], rsp
	lea rax, [rip+capture_return]
	mov qword ptr [r8+8], rax
	fninit
	mov word ptr [rsp+0x48], 0x077f
	fldcw word ptr [rsp+0x48]
	mov dword ptr [rsp+0x4c], 0x3f80
	ldmxcsr dword ptr [rsp+0x4c]
	fld1
	fldz

.macro seed_xmm reg, value
	mov eax, \value
	movd \reg, eax
	punpcklbw \reg, \reg
	punpcklwd \reg, \reg
	pshufd \reg, \reg, 0
.endm
	seed_xmm xmm0, 1
	seed_xmm xmm1, 2
	seed_xmm xmm2, 3
	seed_xmm xmm3, 4
	seed_xmm xmm4, 5
	seed_xmm xmm5, 6
	seed_xmm xmm6, 7
	seed_xmm xmm7, 8
	seed_xmm xmm8, 9
	seed_xmm xmm9, 10
	seed_xmm xmm10, 11
	seed_xmm xmm11, 12
	seed_xmm xmm12, 13
	seed_xmm xmm13, 14
	seed_xmm xmm14, 15
	seed_xmm xmm15, 16
	xor eax, eax
	mov rax, 0x0102030405060708
	mov rcx, qword ptr [rsp+0x30]
	mov rdx, 0x1122334455667788
	mov rbx, 0x2132435465768798
	mov rbp, 0x31425364758697a8
	mov rsi, 0x415263748596a7b8
	mov rdi, 0x5162738495a6b7c8
	mov r8, 0x61728394a5b6c7d8
	mov r9, 0x718293a4b5c6d7e8
	mov r10, 0x8192a3b4c5d6e7f8
	mov r11, 0x91a2b3c4d5e6f708
	mov r12, 0xa1b2c3d4e5f60718
	mov r13, 0xb1c2d3e4f5061728
	mov r14, 0xc1d2e3f405162738
	mov r15, 0xd1e2f30415263748
	stc
	call qword ptr [rsp+0x20]
capture_return:
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

.macro snapshot field, slot
	mov rdx, qword ptr [rsp+\slot]
	mov qword ptr [rax+\field], rdx
.endm
	snapshot WIBO_CTX64_RAX, 112
	snapshot WIBO_CTX64_RCX, 104
	snapshot WIBO_CTX64_RDX, 96
	snapshot WIBO_CTX64_RBX, 88
	snapshot WIBO_CTX64_RBP, 80
	snapshot WIBO_CTX64_RSI, 72
	snapshot WIBO_CTX64_RDI, 64
	snapshot WIBO_CTX64_R8, 56
	snapshot WIBO_CTX64_R9, 48
	snapshot WIBO_CTX64_R10, 40
	snapshot WIBO_CTX64_R11, 32
	snapshot WIBO_CTX64_R12, 24
	snapshot WIBO_CTX64_R13, 16
	snapshot WIBO_CTX64_R14, 8
	snapshot WIBO_CTX64_R15, 0
	lea rdx, [rsp+0x80]
	mov qword ptr [rax+WIBO_CTX64_RSP], rdx
	lea rdx, [rip+capture_return]
	mov qword ptr [rax+WIBO_CTX64_RIP], rdx
	mov edx, dword ptr [rsp+120]
	mov dword ptr [rax+WIBO_CTX64_EFLAGS], edx
	stmxcsr [rax+WIBO_CTX64_MXCSR]
	mov word ptr [rax+WIBO_CTX64_CS], cs
	mov word ptr [rax+WIBO_CTX64_SS], ss
	fxsave [rax+WIBO_CTX64_FXSAVE]
	add rsp, 0x80
	cmp dword ptr [rsp+0x40], 0
	jne probe_complete
	cmp qword ptr [rsp+0x28], 0
	je probe_complete
	mov dword ptr [rsp+0x40], 1
	cmp dword ptr [rsp+0x44], 0
	je restore_prepare
	mov rax, qword ptr [rsp+0x30]
	mov dword ptr [rax+WIBO_CTX64_MXCSR], 0x5f80
restore_prepare:
	fninit
	mov word ptr [rsp+0x48], 0x037f
	fldcw word ptr [rsp+0x48]
	mov dword ptr [rsp+0x4c], 0x1f80
	ldmxcsr dword ptr [rsp+0x4c]

.macro clear_xmm reg
	pxor \reg, \reg
.endm
	clear_xmm xmm0
	clear_xmm xmm1
	clear_xmm xmm2
	clear_xmm xmm3
	clear_xmm xmm4
	clear_xmm xmm5
	clear_xmm xmm6
	clear_xmm xmm7
	clear_xmm xmm8
	clear_xmm xmm9
	clear_xmm xmm10
	clear_xmm xmm11
	clear_xmm xmm12
	clear_xmm xmm13
	clear_xmm xmm14
	clear_xmm xmm15
	xor eax, eax
	xor edx, edx
	xor ebx, ebx
	xor ebp, ebp
	xor esi, esi
	xor edi, edi
	xor r8d, r8d
	xor r9d, r9d
	xor r10d, r10d
	xor r11d, r11d
	xor r12d, r12d
	xor r13d, r13d
	xor r14d, r14d
	xor r15d, r15d
	mov rcx, qword ptr [rsp+0x30]
	call qword ptr [rsp+0x28]
probe_complete:
	fxrstor [rsp+0x50]
	add rsp, PROBE_FRAME_SIZE
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
.size LABEL(wiboContextProbe), .-LABEL(wiboContextProbe)
.section .note.GNU-stack, "", @progbits
#endif
