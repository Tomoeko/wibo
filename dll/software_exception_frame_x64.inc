# Preserve the API caller before changing registers or entering host context.
.macro capture_software_context frame_size
	pushfq
	push rax
	push r11
	lea rsp, [rsp-\frame_size]
	.cfi_def_cfa_offset (\frame_size+32)
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
	mov rax, qword ptr [rsp+\frame_size+8]
	mov qword ptr [r11+WIBO_CTX64_RAX], rax
	mov rax, qword ptr [rsp+\frame_size]
	mov qword ptr [r11+WIBO_CTX64_R11], rax
	lea rax, [rsp+\frame_size+32]
	mov qword ptr [r11+WIBO_CTX64_RSP], rax
	mov rax, qword ptr [rsp+\frame_size+24]
	mov qword ptr [r11+WIBO_CTX64_RIP], rax
	mov eax, dword ptr [rsp+\frame_size+16]
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
