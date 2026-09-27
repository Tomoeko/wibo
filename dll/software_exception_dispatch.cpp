#include "software_exception_dispatch.h"

#ifdef WIBO_GUEST_64
void wiboPrepareSoftwareExceptionDispatchCapture64(SoftwareExceptionCapture64 *capture, BOOL raiseArguments) {
	wiboPrepareSoftwareExceptionCaptureForEntry64(capture, raiseArguments,
												  reinterpret_cast<const void *>(&wiboDispatchRaiseException64),
												  wiboRaiseDispatchContinuation64);
}
#endif
