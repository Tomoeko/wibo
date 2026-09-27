#include "winnt.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "modules.h"

#include <cstring>

namespace kernel32 {

#ifndef WIBO_GUEST_64
void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlUnwind(%p, %p, %p, %p)\n", TargetFrame, TargetIp, ExceptionRecord, ReturnValue);
	DEBUG_LOG("WARNING: Silently returning from RtlUnwind - exception handlers and clean up code may not be run\n");
}
#endif

PVOID WINAPI RtlPcToFileHeader(PVOID PcValue, GUEST_PTR *BaseOfImage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlPcToFileHeader(%p, %p)\n", PcValue, BaseOfImage);
	PVOID base = wibo::loadedImageBaseFromAddress(PcValue);
	if (BaseOfImage) {
		const GUEST_PTR value = base ? toGuestPtr(base) : GUEST_NULL;
		std::memcpy(BaseOfImage, &value, sizeof(value));
	}
	return base;
}

} // namespace kernel32
