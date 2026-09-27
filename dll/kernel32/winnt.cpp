#include "winnt.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "modules.h"

#include <cstdio>

namespace kernel32 {

void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlUnwind(%p, %p, %p, %p)\n", TargetFrame, TargetIp, ExceptionRecord, ReturnValue);
#ifdef WIBO_GUEST_64
	// Returning without unwinding would skip cleanup and resume an invalid state.
	std::fprintf(stderr, "Unsupported target unwind: RtlUnwind\n");
	exitInternal(static_cast<DWORD>(STATUS_NOT_IMPLEMENTED));
#else
	DEBUG_LOG("WARNING: Silently returning from RtlUnwind - exception handlers and clean up code may not be run\n");
#endif
}

PVOID WINAPI RtlPcToFileHeader(PVOID PcValue, GUEST_PTR *BaseOfImage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlPcToFileHeader(%p, %p)\n", PcValue, BaseOfImage);
	wibo::ModuleInfo *module = wibo::moduleInfoFromAddress(PcValue);
	PVOID base = module && module->executable ? module->executable->imageBase : nullptr;
	if (BaseOfImage) {
		*BaseOfImage = base ? toGuestPtr(base) : GUEST_NULL;
	}
	return base;
}

} // namespace kernel32
