	.text
	.p2align 4
	.globl target_unwind_outer
	.def target_unwind_outer; .scl 2; .type 32; .endef
	.seh_proc target_unwind_outer
	.seh_handler target_unwind_outer_handler, @unwind
target_unwind_outer:
	pushq %rbx
	.seh_pushreg %rbx
	pushq %r12
	.seh_pushreg %r12
	subq $88, %rsp
	.seh_stackalloc 88
	movdqa %xmm6, 48(%rsp)
	.seh_savexmm %xmm6, 48
	.seh_endprologue
	movq %rsp, target_unwind_outer_rsp(%rip)
	movq %rcx, 40(%rsp)
	movabsq $0x1122334455667788, %rbx
	movq %rbx, 32(%rsp)
	movabsq $0x33445566778899aa, %r12
	movdqa target_unwind_outer_xmm6(%rip), %xmm6
	movq 40(%rsp), %rcx
	call target_unwind_inner
	.globl target_unwind_outer_after_inner
target_unwind_outer_after_inner:
	movl $1, target_unwind_inner_returned(%rip)
	jmp .Louter_complete
	.globl target_unwind_landing
target_unwind_landing:
	movq %rsp, target_unwind_landed_rsp(%rip)
	movq %rax, target_unwind_landed_rax(%rip)
	movq %rbx, target_unwind_landed_rbx(%rip)
	movq %r12, target_unwind_landed_r12(%rip)
	movdqu %xmm6, target_unwind_landed_xmm6(%rip)
	movl $1, target_unwind_landed(%rip)
.Louter_complete:
	movdqa 48(%rsp), %xmm6
	addq $88, %rsp
	popq %r12
	popq %rbx
	ret
	.seh_handlerdata
	.globl target_unwind_outer_data
target_unwind_outer_data:
	.long 0x27182818
	.long 2
	.text
	.seh_endproc

	.p2align 4
	.globl target_unwind_inner
	.def target_unwind_inner; .scl 2; .type 32; .endef
	.seh_proc target_unwind_inner
	.seh_handler target_unwind_inner_handler, @unwind
target_unwind_inner:
	pushq %r12
	.seh_pushreg %r12
	subq $80, %rsp
	.seh_stackalloc 80
	movdqa %xmm6, 48(%rsp)
	.seh_savexmm %xmm6, 48
	.seh_endprologue
	movq %rsp, target_unwind_inner_rsp(%rip)
	movq %rcx, 40(%rsp)
	movabsq $0x2233445566778899, %r12
	movq %r12, 32(%rsp)
	movdqa target_unwind_inner_xmm6(%rip), %xmm6
	movq 40(%rsp), %rax
	movq target_unwind_outer_rsp(%rip), %rcx
	leaq target_unwind_landing(%rip), %rdx
	leaq target_unwind_record(%rip), %r8
	movq target_unwind_value(%rip), %r9
	call *%rax
	.globl target_unwind_inner_after_call
target_unwind_inner_after_call:
	# An unexpected API return reaches an ordinary failure marker.
	movl $1, target_unwind_returned(%rip)
	movdqa 48(%rsp), %xmm6
	addq $80, %rsp
	popq %r12
	ret
	.seh_handlerdata
	.globl target_unwind_inner_data
target_unwind_inner_data:
	.long 0x27182818
	.long 1
	.text
	.seh_endproc
