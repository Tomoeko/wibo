#include "macros.S"
#include "context_x64_offsets.h"
#include "target_unwind_offsets.h"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text
#include "software_exception_frame_x64.inc"

ASM_GLOBAL(wiboDispatchRtlUnwind64, @function)
	.cfi_startproc
	capture_software_context WIBO_TARGET_UNWIND_FRAME_SIZE
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	lea rsi, [rsp+WIBO_TARGET_UNWIND_FRAME_OUTPUT]
	call SYMBOL_NAME(wiboPrepareTargetUnwind64)
	test eax, eax
	jne 1f
	# The driver returned. The restore entry snapshots the selected state and
	# validates activation transfer before any native scope is abandoned.
	reenter_guest
	lea rcx, [rsp+WIBO_TARGET_UNWIND_FRAME_OUTPUT+WIBO_TARGET_UNWIND_CONTEXT_OFFSET]
	xor edx, edx
	.cfi_remember_state
	call SYMBOL_NAME(wiboRestoreGuestContext64)
1:
	.cfi_restore_state
	lea rdi, [rsp+WIBO_TARGET_UNWIND_FRAME_OUTPUT]
	call SYMBOL_NAME(wiboUnsupportedTargetUnwind64)
	.cfi_endproc
ASM_END(wiboDispatchRtlUnwind64)
#endif
