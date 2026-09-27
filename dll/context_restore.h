#pragma once

#include "context_restore_offsets.h"
#include "software_exception_activation.h"
#include "software_exception_capture.h"

#ifdef WIBO_GUEST_64
enum class ContextRestoreKind64 : DWORD {
	Restore = 0,
	InvalidContext,
	UnsupportedExceptionRecord,
	UnsupportedContextState,
	UnsupportedControlTransfer,
	UnsupportedActivationTransfer,
	Consolidate,
};

struct alignas(16) ContextRestorePreparation64 {
	CONTEXT64 context;
	ContextRestoreKind64 kind;
	DWORD status;
	ULONGLONG reserved;
	EXCEPTION_RECORD *record;
	CONTEXT64 *sourceContext;
	PVOID callback;
	SoftwareExceptionActivation64 activation;
};

static_assert(offsetof(ContextRestorePreparation64, context) == 0);
static_assert(offsetof(ContextRestorePreparation64, kind) == 1232);
static_assert(sizeof(ContextRestorePreparation64) == WIBO_CONTEXT_RESTORE_OUTPUT_SIZE);
static_assert(offsetof(ContextRestorePreparation64, record) == WIBO_CONTEXT_RESTORE_RECORD_OFFSET);
static_assert(offsetof(ContextRestorePreparation64, sourceContext) == WIBO_CONTEXT_RESTORE_SOURCE_OFFSET);
static_assert(offsetof(ContextRestorePreparation64, callback) == WIBO_CONTEXT_RESTORE_CALLBACK_OFFSET);
static_assert(offsetof(ContextRestorePreparation64, activation) == WIBO_CONTEXT_RESTORE_ACTIVATION_OFFSET);
static_assert(static_cast<DWORD>(ContextRestoreKind64::Consolidate) == WIBO_CONTEXT_RESTORE_CONSOLIDATE);
static_assert(WIBO_CONTEXT_RESTORE_FRAME_OUTPUT + sizeof(ContextRestorePreparation64) ==
			  WIBO_CONTEXT_RESTORE_FRAME_SIZE);

extern "C" {
// Full legacy state supports ordinary restoration and consolidation callbacks.
// Long jumps, collided unwinds and extended/debug state remain unsupported.
[[noreturn]] void GUEST_STDCALL wiboRestoreGuestContext64(CONTEXT64 *context, EXCEPTION_RECORD *record);

// Assembly owns a distinct, non-null output. This native helper snapshots and
// validates state, then prepares activation cleanup, and always returns normally.
DWORD wiboPrepareContextRestore64(const SoftwareExceptionCapture64 *entry, ContextRestorePreparation64 *output);
DWORD wiboCompleteContextConsolidation64(ContextRestorePreparation64 *output);
ULONGLONG wiboCallConsolidationHandler64(PVOID callback, EXCEPTION_RECORD *record,
										 SoftwareExceptionActivation64 *activation);
[[noreturn]] void wiboUnsupportedContextRestore64(const ContextRestorePreparation64 *output);
}
#endif
