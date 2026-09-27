#pragma once

#include "context_x64.h"
#include "kernel32/errhandlingapi.h"
#include "software_exception_capture_offsets.h"

#ifdef WIBO_GUEST_64
struct alignas(16) SoftwareExceptionCapture64 {
	CONTEXT64 context;
	EXCEPTION_RECORD localRecord;
	EXCEPTION_RECORD *record;
	const SoftwareExceptionCapture64 *callerCapture;
};

static_assert(sizeof(EXCEPTION_RECORD) == 152);
static_assert(offsetof(EXCEPTION_RECORD, ExceptionInformation) == 32);
static_assert(sizeof(EXCEPTION_POINTERS) == 16);
static_assert(offsetof(SoftwareExceptionCapture64, context) == WIBO_SOFTWARE_CAPTURE_CONTEXT);
static_assert(offsetof(SoftwareExceptionCapture64, localRecord) == WIBO_SOFTWARE_CAPTURE_LOCAL_RECORD);
static_assert(offsetof(SoftwareExceptionCapture64, record) == WIBO_SOFTWARE_CAPTURE_RECORD);
static_assert(offsetof(SoftwareExceptionCapture64, callerCapture) == WIBO_SOFTWARE_CAPTURE_CALLER);
static_assert(sizeof(SoftwareExceptionCapture64) == WIBO_SOFTWARE_CAPTURE_SIZE);

extern "C" {

// Internal capture entries, not public exception-dispatch exports. The consumer
// must return normally and must not mutate the captured register snapshots.
void GUEST_STDCALL wiboCaptureRaiseException64(DWORD code, DWORD flags, DWORD count, const ULONG_PTR *arguments);
void GUEST_STDCALL wiboCaptureRtlRaiseException64(EXCEPTION_RECORD *record);
extern const BYTE wiboRaiseCaptureContinuation64[];

void wiboPrepareSoftwareExceptionCapture64(SoftwareExceptionCapture64 *capture, BOOL raiseArguments);

// Deliberately has no default implementation: an actual capture consumer must
// be supplied before these entry points can be linked into a runnable target.
// Capture frames and their caller links remain owned by the assembly entries.
void wiboConsumeSoftwareExceptionCapture64(const SoftwareExceptionCapture64 *capture);

// Native SysV entry. Preserves nonvolatile registers and legacy floating-point
// state even when a guest callback changes them before returning normally.
LONG wiboCallVectoredHandler64(PVECTORED_EXCEPTION_HANDLER handler, PEXCEPTION_POINTERS exceptionInfo);
}

namespace wibo {
LONG invokeVectoredGuestHandler64(PVECTORED_EXCEPTION_HANDLER handler, PEXCEPTION_POINTERS exceptionInfo);
}
#endif
