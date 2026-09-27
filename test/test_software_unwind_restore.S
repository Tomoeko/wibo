.intel_syntax noprefix
.text
.p2align 4
.globl softwareUnwindRestoreProbe
.seh_proc softwareUnwindRestoreProbe
.seh_handler software_unwind_outer_handler, @except, @unwind
softwareUnwindRestoreProbe:
	push rbx
	.seh_pushreg rbx
	push r12
	.seh_pushreg r12
	sub rsp, 88
	.seh_stackalloc 88
	movdqa xmmword ptr [rsp+48], xmm6
	.seh_savexmm xmm6, 48
	.seh_endprologue
	mov r12, rdx
	mov qword ptr [rsp+40], rdx
	mov qword ptr [r12], rsp
	mov rbx, 0x1122334455667788
	mov qword ptr [rsp+32], rbx
	movdqa xmm6, xmmword ptr [rip+software_unwind_outer_xmm6]
	call softwareUnwindRestoreInner
	mov rdx, qword ptr [rsp+40]
	mov dword ptr [rdx+68], 1
	jmp .Lsoftware_restore_complete
.globl software_unwind_restore_failure
software_unwind_restore_failure:
	# An unsuccessful transfer has a valid failure continuation.
	mov rdx, qword ptr [rsp+40]
	mov dword ptr [rdx+72], 1
	jmp .Lsoftware_restore_complete
.globl software_unwind_restore_landing
software_unwind_restore_landing:
	# Use the retained frame slot, then snapshot before changing selected values.
	mov rdx, qword ptr [rsp+40]
	mov qword ptr [rdx+16], rsp
	mov qword ptr [rdx+24], rax
	mov qword ptr [rdx+32], rbx
	mov qword ptr [rdx+40], r12
	movdqu xmmword ptr [rdx+48], xmm6
	mov dword ptr [rdx+64], 1
	imul eax, dword ptr [rdx+96], 10
	add eax, 4
	mov dword ptr [rdx+96], eax
.Lsoftware_restore_complete:
	movdqa xmm6, xmmword ptr [rsp+48]
	add rsp, 88
	pop r12
	pop rbx
	ret
.seh_handlerdata
	.long 0x31415926
.text
.seh_endproc

.p2align 4
.globl softwareUnwindRestoreInner
.seh_proc softwareUnwindRestoreInner
.seh_handler software_unwind_inner_handler, @unwind
softwareUnwindRestoreInner:
	push r12
	.seh_pushreg r12
	sub rsp, 80
	.seh_stackalloc 80
	.seh_endprologue
	mov qword ptr [rdx+8], rsp
	mov qword ptr [rsp+40], rdx
	mov rax, rcx
	mov ecx, 0xe0420a03
	xor edx, edx
	xor r8d, r8d
	xor r9d, r9d
	call rax
	# Keep the call continuation in the body so its unwind handler remains active.
	nop
	# If Raise returns, the outer frame records an ordinary failure.
	add rsp, 80
	pop r12
	ret
.seh_handlerdata
	.long 0x27182818
.text
.seh_endproc
