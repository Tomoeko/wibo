#ifdef __APPLE__
#define LABEL_(name) _##name
#define LABEL(name) LABEL_(name)
#else
#define LABEL(name) name
#endif

.intel_syntax noprefix
.text
.globl LABEL(wiboSoftwareCaptureProbe64)
#ifdef __ELF__
.type LABEL(wiboSoftwareCaptureProbe64), @function
#endif
LABEL(wiboSoftwareCaptureProbe64):
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	# Call area, pointers, a local record, and original floating-point state.
	sub rsp, 0x2e8
	mov qword ptr [rsp+0x20], rcx
	mov qword ptr [rsp+0x28], rdx
	mov qword ptr [rsp+0x30], r8
	mov qword ptr [rsp+0x38], r9
	fxsave [rsp+0xe0]
	mov rbx, 0x2132435465768798
	mov rbp, 0x31425364758697a8
	mov r12, 0xa1b2c3d4e5f60718
	mov r13, 0xb1c2d3e4f5061728
	mov r14, 0xc1d2e3f405162738
	mov r15, 0xd1e2f30415263748
	mov eax, 0x26262626
	movd xmm6, eax
	pshufd xmm6, xmm6, 0
	mov eax, 0x3f3f3f3f
	movd xmm15, eax
	pshufd xmm15, xmm15, 0
	lea rax, [rip+raise_return]
	mov qword ptr [r9], rax
	mov qword ptr [r9+8], rsp
	mov ecx, 0xe0420601
	mov edx, 0x80
	mov r8d, 15
	mov r9, qword ptr [rsp+0x30]
	call qword ptr [rsp+0x20]
raise_return:
	lea rdi, [rsp+0x40]
	xor eax, eax
	mov ecx, 19
	cld
	rep stosq
	lea rcx, [rsp+0x40]
	mov dword ptr [rcx], 0xe0420601
	mov qword ptr [rcx+16], 0x1234
	mov dword ptr [rcx+24], 2
	mov qword ptr [rcx+32], 0x4567
	mov qword ptr [rcx+40], 0x89ab
	mov rax, qword ptr [rsp+0x38]
	lea rdx, [rip+rtl_return]
	mov qword ptr [rax+16], rdx
	mov qword ptr [rax+24], rsp
	mov qword ptr [rax+32], rcx
	call qword ptr [rsp+0x28]
rtl_return:
	mov rax, qword ptr [rsp+0x38]
	mov rdx, qword ptr [rsp+0x40+16]
	mov qword ptr [rax+40], rdx
	fxrstor [rsp+0xe0]
	add rsp, 0x2e8
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
.size LABEL(wiboSoftwareCaptureProbe64), .-LABEL(wiboSoftwareCaptureProbe64)
#endif

.macro seed_callback_xmm reg, value
	mov eax, \value
	movd \reg, eax
	punpcklbw \reg, \reg
	punpcklwd \reg, \reg
	pshufd \reg, \reg, 0
.endm

.globl LABEL(wiboVectoredAdapterProbe64)
LABEL(wiboVectoredAdapterProbe64):
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	sub rsp, 0x258
	mov qword ptr [rsp+0x20], rcx
	mov qword ptr [rsp+0x28], rdx
	mov qword ptr [rsp+0x30], r8
	mov qword ptr [rsp+0x38], r9
	fxsave [rsp+0x50]
	fninit
	mov word ptr [rsp+0x48], 0x077f
	fldcw word ptr [rsp+0x48]
	mov dword ptr [rsp+0x4c], 0x3f80
	ldmxcsr dword ptr [rsp+0x4c]
	fld1
	fldz
	seed_callback_xmm xmm0, 1
	seed_callback_xmm xmm1, 2
	seed_callback_xmm xmm2, 3
	seed_callback_xmm xmm3, 4
	seed_callback_xmm xmm4, 5
	seed_callback_xmm xmm5, 6
	seed_callback_xmm xmm6, 7
	seed_callback_xmm xmm7, 8
	seed_callback_xmm xmm8, 9
	seed_callback_xmm xmm9, 10
	seed_callback_xmm xmm10, 11
	seed_callback_xmm xmm11, 12
	seed_callback_xmm xmm12, 13
	seed_callback_xmm xmm13, 14
	seed_callback_xmm xmm14, 15
	seed_callback_xmm xmm15, 16
	mov rbx, 0x2132435465768798
	mov rbp, 0x31425364758697a8
	mov r12, 0xa1b2c3d4e5f60718
	mov r13, 0xb1c2d3e4f5061728
	mov r14, 0xc1d2e3f405162738
	mov r15, 0xd1e2f30415263748
	mov rdi, qword ptr [rsp+0x28]
	mov rsi, qword ptr [rsp+0x30]
	call qword ptr [rsp+0x20]
	mov edx, eax
	mov rax, qword ptr [rsp+0x38]
	mov qword ptr [rax], rdx
	mov qword ptr [rax+144], rbx
	mov qword ptr [rax+160], rbp
	mov qword ptr [rax+168], rsi
	mov qword ptr [rax+176], rdi
	mov qword ptr [rax+216], r12
	mov qword ptr [rax+224], r13
	mov qword ptr [rax+232], r14
	mov qword ptr [rax+240], r15
	stmxcsr [rax+52]
	fxsave [rax+256]
	fxrstor [rsp+0x50]
	add rsp, 0x258
	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	ret

.globl LABEL(wiboChangingVectoredHandler64)
LABEL(wiboChangingVectoredHandler64):
	# A synthetic callback changes registers while leaving the stack intact, so
	# the isolated preservation probe can report mismatches on a normal return.
	xor ebx, ebx
	xor ebp, ebp
	xor esi, esi
	xor edi, edi
	xor r12d, r12d
	xor r13d, r13d
	xor r14d, r14d
	xor r15d, r15d
	pxor xmm0, xmm0
	pxor xmm1, xmm1
	pxor xmm2, xmm2
	pxor xmm3, xmm3
	pxor xmm4, xmm4
	pxor xmm5, xmm5
	pxor xmm6, xmm6
	pxor xmm7, xmm7
	pxor xmm8, xmm8
	pxor xmm9, xmm9
	pxor xmm10, xmm10
	pxor xmm11, xmm11
	pxor xmm12, xmm12
	pxor xmm13, xmm13
	pxor xmm14, xmm14
	pxor xmm15, xmm15
	fninit
	mov eax, -1
	ret
#ifdef __ELF__
.section .note.GNU-stack, "", @progbits
#endif
