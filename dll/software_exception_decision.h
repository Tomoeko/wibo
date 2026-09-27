#pragma once

#include "software_exception_capture.h"

#ifdef WIBO_GUEST_64
enum class SoftwareExceptionDecisionKind64 : DWORD {
	Resume = 0,
	UnsupportedFrameDispatch,
	NoncontinuableContinuationRequiresDispatch,
	UnsupportedControlTransfer,
	UnsupportedContextState,
	InvalidCapture,
	LegacyNotificationReturn,
};

struct alignas(16) SoftwareExceptionDecision64 {
	CONTEXT64 resumeContext;
	SoftwareExceptionDecisionKind64 kind;
	DWORD failureCode;
	DWORD originalCode;
	DWORD originalFlags;
};

static_assert(offsetof(SoftwareExceptionDecision64, resumeContext) == 0);
static_assert(offsetof(SoftwareExceptionDecision64, kind) == 1232);
static_assert(sizeof(SoftwareExceptionDecision64) == 1248);

// Native host entry with normal-return callbacks only. Output must be non-null
// and must not overlap the capture, its record, or linked capture frames.
// This helper never restores context.
// A Resume result covers only the supported decision subset; frame dispatch,
// continuation execution, and nonlocal callback cleanup remain separate work.
// LegacyNotificationReturn preserves the existing notification policy without
// treating unsuccessful handler traversal as a Resume result.
extern "C" DWORD wiboPrepareSoftwareExceptionDecision64(const SoftwareExceptionCapture64 *capture,
														SoftwareExceptionDecision64 *output,
														bool deferNotificationFallback = false);
// Shared validation for a normal-return handler selecting the original context.
void wiboSelectSoftwareExceptionContinuation64(const SoftwareExceptionCapture64 *capture,
											   SoftwareExceptionDecision64 *output);
#endif
