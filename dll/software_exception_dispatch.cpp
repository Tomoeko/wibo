#include "software_exception_dispatch.h"

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
	if (decision->kind == SoftwareExceptionDecisionKind64::UnsupportedFrameDispatch ||
		decision->kind == SoftwareExceptionDecisionKind64::LegacyNotificationReturn) {
		wiboSearchSoftwareExceptionFrames64(capture, decision, activation);
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
