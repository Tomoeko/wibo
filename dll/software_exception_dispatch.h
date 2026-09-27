#pragma once

#include "software_exception_decision.h"
#include "software_exception_dispatch_offsets.h"

#ifdef WIBO_GUEST_64
static_assert(sizeof(SoftwareExceptionDecision64) == WIBO_SOFTWARE_DISPATCH_DECISION_SIZE);
static_assert(static_cast<DWORD>(SoftwareExceptionDecisionKind64::Resume) == WIBO_SOFTWARE_DISPATCH_RESUME);
static_assert(static_cast<DWORD>(SoftwareExceptionDecisionKind64::LegacyNotificationReturn) ==
			  WIBO_SOFTWARE_DISPATCH_NOTIFICATION_RETURN);
static_assert(WIBO_SOFTWARE_DISPATCH_FRAME_DECISION + sizeof(SoftwareExceptionDecision64) <=
			  WIBO_SOFTWARE_DISPATCH_FRAME_SIZE);

extern "C" {
// Guest entries for fixed-continuation, normal-return vectored handling.
// General frame dispatch and arbitrary context transfers remain unsupported.
void GUEST_STDCALL wiboDispatchRaiseException64(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments);
void GUEST_STDCALL wiboDispatchRtlRaiseException64(EXCEPTION_RECORD *record);
extern const BYTE wiboRaiseDispatchContinuation64[];

void wiboPrepareSoftwareExceptionDispatchCapture64(SoftwareExceptionCapture64 *capture, BOOL raiseArguments);

// No default implementation. The consumer is entered in host context after
// decision traversal has returned and must not resume or return to this entry.
[[noreturn]] void wiboUnsupportedSoftwareExceptionDispatch64(const SoftwareExceptionCapture64 *capture,
															 const SoftwareExceptionDecision64 *decision);
}
#endif
