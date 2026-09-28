#include "software_exception_dispatch.h"

#include "kernel32/errhandlingapi.h"

#include <cstring>

#ifdef WIBO_GUEST_64
void wiboPrepareSoftwareExceptionDispatchCapture64(SoftwareExceptionCapture64 *capture, BOOL raiseArguments) {
	wiboPrepareSoftwareExceptionCaptureForEntry64(capture, raiseArguments,
												  reinterpret_cast<const void *>(&wiboDispatchRaiseException64),
												  wiboRaiseDispatchContinuation64);
}

DWORD wiboPrepareSoftwareExceptionDispatchDecision64(const SoftwareExceptionCapture64 *capture,
													 SoftwareExceptionDecision64 *decision,
													 SoftwareExceptionFrameActivation64 *activation) {
	wiboPrepareSoftwareExceptionDecision64(capture, decision, true);
	bool framesExhausted = false;
	if (decision->kind == SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch ||
		decision->kind == SoftwareExceptionDecisionKind64::LegacyNotificationReturn) {
		framesExhausted = wiboSearchSoftwareExceptionFrames64(capture, decision, activation);
	}
	if (framesExhausted && decision->kind == SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch) {
		if (const auto filter = kernel32::currentUnhandledExceptionFilter()) {
			EXCEPTION_POINTERS info{toGuestPtr(capture->record), toGuestPtr(&decision->resumeContext)};
			const LONG result = wibo::invokeVectoredGuestHandler64(filter, &info);
			if (result == EXCEPTION_CONTINUE_EXECUTION) {
				wiboSelectSoftwareExceptionContinuation64(capture, decision);
				if (decision->kind == SoftwareExceptionDecisionKind64::Resume &&
					(info.ExceptionRecord != toGuestPtr(capture->record) ||
					 info.ContextRecord != toGuestPtr(&decision->resumeContext))) {
					decision->kind = SoftwareExceptionDecisionKind64::UnsupportedContextState;
					decision->failureCode = decision->originalCode;
				}
			}
		}
	}
	if (decision->kind == SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch)
		decision->failureCode = decision->originalCode;
	if (decision->kind == SoftwareExceptionDecisionKind64::LegacyNotificationReturn) {
		if ((decision->originalFlags | capture->record->ExceptionFlags) & 1) {
			decision->kind = SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch;
			decision->failureCode = decision->originalCode;
		} else {
			std::memcpy(&decision->resumeContext, &capture->context, sizeof(CONTEXT64));
		}
	}
	return static_cast<DWORD>(decision->kind);
}
#endif
