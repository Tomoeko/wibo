#include "software_exception_capture.h"

#include "setup.h"

#include <algorithm>

#ifdef WIBO_GUEST_64
void wiboPrepareSoftwareExceptionCapture64(SoftwareExceptionCapture64 *capture, BOOL raiseArguments) {
	wiboPrepareSoftwareExceptionCaptureForEntry64(capture, raiseArguments,
												  reinterpret_cast<const void *>(&wiboCaptureRaiseException64),
												  wiboRaiseCaptureContinuation64);
}

void wiboPrepareSoftwareExceptionCaptureForEntry64(SoftwareExceptionCapture64 *capture, BOOL raiseArguments,
												   const void *raiseEntry, const void *raiseContinuation) {
	capture->callerCapture = nullptr;
	if (raiseArguments) {
		capture->localRecord = {};
		auto &record = capture->localRecord;
		record.ExceptionCode = static_cast<DWORD>(capture->context.Rcx);
		record.ExceptionFlags = static_cast<DWORD>(capture->context.Rdx) & 1;
		record.ExceptionAddress = toGuestPtr(raiseEntry);
		const auto *arguments = fromGuestPtr<const ULONG_PTR>(capture->context.R9);
		if (arguments) {
			record.NumberParameters = std::min(static_cast<DWORD>(capture->context.R8), EXCEPTION_MAXIMUM_PARAMETERS);
			std::copy_n(arguments, record.NumberParameters, record.ExceptionInformation);
		}
		capture->record = &record;
		return;
	}
	capture->record = fromGuestPtr<EXCEPTION_RECORD>(capture->context.Rcx);
	capture->record->ExceptionAddress = capture->context.Rip;
	if (capture->context.Rip == toGuestPtr(raiseContinuation)) {
		const auto *caller =
			fromGuestPtr<const SoftwareExceptionCapture64>(capture->context.Rsp + WIBO_SOFTWARE_FRAME_CAPTURE);
		if (capture->record == &caller->localRecord) {
			capture->callerCapture = caller;
		}
	}
}

namespace wibo {
LONG invokeVectoredGuestHandler64(PVECTORED_EXCEPTION_HANDLER handler, PEXCEPTION_POINTERS exceptionInfo) {
#if defined(__APPLE__)
	TEB *teb = currentTebForGuestTransition();
	enterGuestContext(teb);
#endif
	const LONG result = wiboCallVectoredHandler64(handler, exceptionInfo);
#if defined(__APPLE__)
	enterHostContext();
#endif
	return result;
}
} // namespace wibo
#endif
