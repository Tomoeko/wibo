#include "macros.S"
#include "context_x64_offsets.h"
#include "context_restore_offsets.h"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text
#include "software_exception_frame_x64.inc"

ASM_GLOBAL(wiboRestoreGuestContext64, @function)
	.cfi_startproc
	capture_software_context WIBO_CONTEXT_RESTORE_FRAME_SIZE
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	lea rsi, [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT]
	call SYMBOL_NAME(wiboPrepareContextRestore64)
	test eax, eax
	je 2f
	cmp eax, WIBO_CONTEXT_RESTORE_CONSOLIDATE
	jne 1f
	reenter_guest
	mov rdi, qword ptr [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT+WIBO_CONTEXT_RESTORE_CALLBACK_OFFSET]
	mov rsi, qword ptr [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT+WIBO_CONTEXT_RESTORE_RECORD_OFFSET]
	lea rdx, [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT+WIBO_CONTEXT_RESTORE_ACTIVATION_OFFSET]
	call SYMBOL_NAME(wiboCallConsolidationHandler64)
	mov qword ptr [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT+WIBO_CTX64_RIP], rax
#ifdef __APPLE__
	call SYMBOL_NAME(enterHostContext)
#endif
	lea rdi, [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT]
	call SYMBOL_NAME(wiboCompleteContextConsolidation64)
	test eax, eax
	jne 1f
2:
	# Preparation and activation cleanup returned with no host scope remaining.
	reenter_guest
	lea rcx, [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT]
	.cfi_remember_state
	.cfi_undefined rip
	jmp SYMBOL_NAME(wiboRestoreContext64)
1:
	.cfi_restore_state
	lea rdi, [rsp+WIBO_CONTEXT_RESTORE_FRAME_OUTPUT]
	call SYMBOL_NAME(wiboUnsupportedContextRestore64)
	.cfi_endproc
ASM_END(wiboRestoreGuestContext64)
#endif
