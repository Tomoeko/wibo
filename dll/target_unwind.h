#pragma once

#include "software_exception_frame.h"
#include "target_unwind_offsets.h"

#ifdef WIBO_GUEST_64
enum class TargetUnwindKind64 : DWORD {
	Restore = 0,
	Unsupported,
};

struct alignas(16) TargetUnwindPreparation64 {
	SoftwareExceptionFrameActivation64 frames;
	CONTEXT64 resumeContext;
	EXCEPTION_RECORD localRecord;
	EXCEPTION_RECORD *record;
	ULONGLONG targetFrame;
	ULONGLONG targetIp;
	ULONGLONG returnValue;
	TargetUnwindKind64 kind;
	DWORD status;
};

static_assert(sizeof(TargetUnwindPreparation64) == WIBO_TARGET_UNWIND_OUTPUT_SIZE);
static_assert(offsetof(TargetUnwindPreparation64, resumeContext) == WIBO_TARGET_UNWIND_CONTEXT_OFFSET);
static_assert(WIBO_TARGET_UNWIND_FRAME_OUTPUT + sizeof(TargetUnwindPreparation64) == WIBO_TARGET_UNWIND_FRAME_SIZE);

extern "C" {
// Explicit software target unwind. Exit unwinds, collided unwinds and special
// restoration records remain outside this initial supported subset.
[[noreturn]] void GUEST_STDCALL wiboDispatchRtlUnwind64(PVOID targetFrame, PVOID targetIp, EXCEPTION_RECORD *record,
														PVOID returnValue);
[[noreturn]] void GUEST_STDCALL wiboDispatchRtlUnwindEx64(PVOID targetFrame, PVOID targetIp, EXCEPTION_RECORD *record,
														  PVOID returnValue, CONTEXT64 *context, PVOID historyTable);
DWORD wiboPrepareTargetUnwind64(const SoftwareExceptionCapture64 *entry, TargetUnwindPreparation64 *output);
DWORD wiboPrepareTargetUnwindEx64(const SoftwareExceptionCapture64 *entry, TargetUnwindPreparation64 *output);
[[noreturn]] void wiboUnsupportedTargetUnwind64(const TargetUnwindPreparation64 *output);
}
#endif
