#include "macros.S"
#include "context_x64_offsets.h"
#include "software_exception_dispatch_offsets.h"

#if defined(__x86_64__) && defined(WIBO_GUEST_64)
.text
#include "software_exception_frame_x64.inc"

#ifdef __APPLE__
#define RAISE_DISPATCH_CONTINUATION LwiboRaiseDispatchContinuation
#define DISPATCH_UNSUPPORTED LwiboDispatchUnsupported
#define DISPATCH_RESTORE LwiboDispatchRestore
#else
#define RAISE_DISPATCH_CONTINUATION .LwiboRaiseDispatchContinuation
#define DISPATCH_UNSUPPORTED .LwiboDispatchUnsupported
#define DISPATCH_RESTORE .LwiboDispatchRestore
#endif

ASM_GLOBAL(wiboDispatchRaiseException64, @function)
	.cfi_startproc
	capture_software_context WIBO_SOFTWARE_FRAME_SIZE
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	mov esi, 1
	call SYMBOL_NAME(wiboPrepareSoftwareExceptionDispatchCapture64)
	reenter_guest
	restore_software_registers
	lea rcx, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE+WIBO_SOFTWARE_CAPTURE_LOCAL_RECORD]
	call SYMBOL_NAME(wiboDispatchRtlRaiseException64)
RAISE_DISPATCH_CONTINUATION:
	# The selected state and guest context are already restored by the inner
	# entry. Drop the outer frame without modifying any register or flags.
	lea rsp, [rsp+WIBO_SOFTWARE_FRAME_SIZE+24]
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc
ASM_END(wiboDispatchRaiseException64)
	.globl SYMBOL_NAME(wiboRaiseDispatchContinuation64)
	.set SYMBOL_NAME(wiboRaiseDispatchContinuation64), RAISE_DISPATCH_CONTINUATION

ASM_GLOBAL(wiboDispatchRtlRaiseException64, @function)
	.cfi_startproc
	capture_software_context WIBO_SOFTWARE_DISPATCH_FRAME_SIZE
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	xor esi, esi
	call SYMBOL_NAME(wiboPrepareSoftwareExceptionDispatchCapture64)
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	lea rsi, [rsp+WIBO_SOFTWARE_DISPATCH_FRAME_DECISION]
	lea rdx, [rsp+WIBO_SOFTWARE_DISPATCH_FRAME_ACTIVATION]
	call SYMBOL_NAME(wiboPrepareSoftwareExceptionDispatchDecision64)
	cmp eax, WIBO_SOFTWARE_DISPATCH_RESUME
	je DISPATCH_RESTORE
	cmp eax, WIBO_SOFTWARE_DISPATCH_NOTIFICATION_RETURN
	jne DISPATCH_UNSUPPORTED
DISPATCH_RESTORE:
	# The helper and every callback returned normally. No traversal lock or
	# reference from this dispatch remains live across the restoration.
	reenter_guest
	lea rcx, [rsp+WIBO_SOFTWARE_DISPATCH_FRAME_DECISION]
	.cfi_remember_state
	.cfi_undefined rip
	jmp SYMBOL_NAME(wiboRestoreContext64)
DISPATCH_UNSUPPORTED:
	.cfi_restore_state
	lea rdi, [rsp+WIBO_SOFTWARE_FRAME_CAPTURE]
	lea rsi, [rsp+WIBO_SOFTWARE_DISPATCH_FRAME_DECISION]
	call SYMBOL_NAME(wiboUnsupportedSoftwareExceptionDispatch64)
	.cfi_endproc
ASM_END(wiboDispatchRtlRaiseException64)
#endif
