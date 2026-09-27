#include "test_target_unwind.S"
	.text
	.p2align 4
	.globl target_unwind_ex_adapter
	.def target_unwind_ex_adapter; .scl 2; .type 32; .endef
	.seh_proc target_unwind_ex_adapter
target_unwind_ex_adapter:
	# Retain the verified frames while supplying the two additional ABI arguments.
	subq $56, %rsp
	.seh_stackalloc 56
	.seh_endprologue
	movq target_unwind_ex_context(%rip), %rax
	movq %rax, 32(%rsp)
	movq target_unwind_ex_history(%rip), %rax
	movq %rax, 40(%rsp)
	call *target_unwind_ex_entry(%rip)
	nop
	# A returning unwind reaches a normal failure marker.
	movl $1, target_unwind_returned(%rip)
	addq $56, %rsp
	ret
	.seh_endproc
