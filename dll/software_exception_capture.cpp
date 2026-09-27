#include "software_exception_capture.h"

#ifdef WIBO_GUEST_64
void wiboPrepareSoftwareExceptionCapture64(SoftwareExceptionCapture64 *capture, BOOL raiseArguments) {
	wiboPrepareSoftwareExceptionCaptureForEntry64(capture, raiseArguments,
												  reinterpret_cast<const void *>(&wiboCaptureRaiseException64),
												  wiboRaiseCaptureContinuation64);
}

#endif
