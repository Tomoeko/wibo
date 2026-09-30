#pragma once

#include "errhandlingapi.h"
#include "thread_context.h"
#include "types.h"

namespace kernel32 {

#ifdef WIBO_GUEST_64
void CDECL RtlRestoreContext(CONTEXT64 *context, EXCEPTION_RECORD *record)
	WIBO_ANNOTATE("GUEST_ENTRY:wiboRestoreGuestContext64");
void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
	WIBO_ANNOTATE("GUEST_ENTRY:wiboDispatchRtlUnwind64");
void WINAPI RtlUnwindEx(PVOID targetFrame, PVOID targetIp, EXCEPTION_RECORD *record, PVOID returnValue,
						CONTEXT64 *context, PVOID historyTable) WIBO_ANNOTATE("GUEST_ENTRY:wiboDispatchRtlUnwindEx64");
#else
void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
	WIBO_ANNOTATE("GUEST_ENTRY:wiboRtlUnwind32");
#endif
PVOID WINAPI RtlPcToFileHeader(PVOID PcValue, GUEST_PTR *BaseOfImage);

} // namespace kernel32
