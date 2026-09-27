	.text
	.p2align 4
	.globl frame_outer
	.def frame_outer; .scl 2; .type 32; .endef
	.seh_proc frame_outer
	.seh_handler frame_outer_handler, @except
frame_outer:
	pushq %rbx
	.seh_pushreg %rbx
	subq $48, %rsp
	.seh_stackalloc 48
	.seh_endprologue
	movq %rbx, frame_outer_saved_rbx(%rip)
	movq %rsp, frame_outer_rsp(%rip)
	movq %rcx, 40(%rsp)
	movabsq $0x1122334455667788, %rbx
	movq %rbx, 32(%rsp)
	movq 40(%rsp), %rcx
	call frame_inner
	.globl frame_outer_after_inner
frame_outer_after_inner:
	movl $1, frame_outer_resumed(%rip)
	movq %rsp, frame_outer_resumed_rsp(%rip)
	addq $48, %rsp
	popq %rbx
	ret
	.seh_handlerdata
	.globl frame_outer_handler_data
frame_outer_handler_data:
	.long 0x31415926
	.long 2
	.text
	.seh_endproc

	.p2align 4
	.globl frame_inner
	.def frame_inner; .scl 2; .type 32; .endef
	.seh_proc frame_inner
	.seh_handler frame_inner_handler, @except
frame_inner:
	pushq %r12
	.seh_pushreg %r12
	subq $48, %rsp
	.seh_stackalloc 48
	.seh_endprologue
	movq %r12, frame_inner_saved_r12(%rip)
	movq %rsp, frame_inner_rsp(%rip)
	movq %rcx, 40(%rsp)
	movabsq $0x2233445566778899, %r12
	movq %r12, 32(%rsp)
	movq 40(%rsp), %rax
	movl $0xe0420a01, %ecx
	xorl %edx, %edx
	movl $2, %r8d
	leaq frame_arguments(%rip), %r9
	call *%rax
	.globl frame_inner_after_raise
frame_inner_after_raise:
	movl $1, frame_inner_resumed(%rip)
	movq %rsp, frame_inner_resumed_rsp(%rip)
	addq $48, %rsp
	popq %r12
	ret
	.seh_handlerdata
	.globl frame_inner_handler_data
frame_inner_handler_data:
	.long 0x31415926
	.long 1
	.text
	.seh_endproc
