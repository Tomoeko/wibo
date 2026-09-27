#pragma once

#include "context_restore_offsets.h"
#include "software_exception_capture.h"

#ifdef WIBO_GUEST_64
enum class ContextRestoreKind64 : DWORD {
	Restore = 0,
	InvalidContext,
	UnsupportedExceptionRecord,
	UnsupportedContextState,
	UnsupportedControlTransfer,
	UnsupportedActivationTransfer,
};

struct alignas(16) ContextRestorePreparation64 {
	CONTEXT64 context;
	ContextRestoreKind64 kind;
	DWORD status;
};

static_assert(offsetof(ContextRestorePreparation64, context) == 0);
static_assert(offsetof(ContextRestorePreparation64, kind) == 1232);
static_assert(sizeof(ContextRestorePreparation64) == WIBO_CONTEXT_RESTORE_OUTPUT_SIZE);
static_assert(WIBO_CONTEXT_RESTORE_FRAME_OUTPUT + sizeof(ContextRestorePreparation64) ==
			  WIBO_CONTEXT_RESTORE_FRAME_SIZE);

extern "C" {
// The public entry accepts only the verified full legacy, NULL-record subset.
// Long jumps, consolidation callbacks and extended/debug state remain unsupported.
[[noreturn]] void GUEST_STDCALL wiboRestoreGuestContext64(CONTEXT64 *context, EXCEPTION_RECORD *record);

// Assembly owns a distinct, non-null output. This native helper snapshots and
// validates state, then prepares activation cleanup, and always returns normally.
DWORD wiboPrepareContextRestore64(const SoftwareExceptionCapture64 *entry, ContextRestorePreparation64 *output);
[[noreturn]] void wiboUnsupportedContextRestore64(const ContextRestorePreparation64 *output);
}
#endif
