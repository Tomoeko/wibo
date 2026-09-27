#include "macros.S"
#include "context_x64_offsets.h"
#include "target_unwind_offsets.h"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text
#include "software_exception_frame_x64.inc"

.macro dispatch_target_unwind prepare
	capture_software_context WIBO_TARGET_UNWIND_FRAME_SIZE
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	lea rsi, [rsp+WIBO_TARGET_UNWIND_FRAME_OUTPUT]
	call \prepare
	test eax, eax
	jne 1f
	# The driver returned. The restore entry snapshots the selected state and
	# validates activation transfer before any native scope is abandoned.
	reenter_guest
	mov rcx, qword ptr [rsp+WIBO_TARGET_UNWIND_FRAME_OUTPUT+WIBO_TARGET_UNWIND_RESTORE_CONTEXT_OFFSET]
	mov rdx, qword ptr [rsp+WIBO_TARGET_UNWIND_FRAME_OUTPUT+WIBO_TARGET_UNWIND_RECORD_OFFSET]
	cmp dword ptr [rdx], 0x80000029
	je 2f
	xor edx, edx
2:
	.cfi_remember_state
	call SYMBOL_NAME(wiboRestoreGuestContext64)
1:
	.cfi_restore_state
	lea rdi, [rsp+WIBO_TARGET_UNWIND_FRAME_OUTPUT]
	call SYMBOL_NAME(wiboUnsupportedTargetUnwind64)
.endm

ASM_GLOBAL(wiboDispatchRtlUnwind64, @function)
	.cfi_startproc
	dispatch_target_unwind SYMBOL_NAME(wiboPrepareTargetUnwind64)
	.cfi_endproc
ASM_END(wiboDispatchRtlUnwind64)

ASM_GLOBAL(wiboDispatchRtlUnwindEx64, @function)
	.cfi_startproc
	dispatch_target_unwind SYMBOL_NAME(wiboPrepareTargetUnwindEx64)
	.cfi_endproc
ASM_END(wiboDispatchRtlUnwindEx64)
#endif
