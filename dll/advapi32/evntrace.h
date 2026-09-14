#pragma once

#include "types.h"

using TRACEGUID_HANDLE = ULONGLONG;

// These are the two requests in the classic trace-provider callback contract.
enum WMIDPREQUESTCODE : ULONG {
	WMI_ENABLE_EVENTS = 4,
	WMI_DISABLE_EVENTS = 5,
};

typedef ULONG(_CC_STDCALL *WMIDPREQUEST)(WMIDPREQUESTCODE RequestCode, PVOID RequestContext, ULONG *BufferSize,
										 PVOID Buffer);

struct TRACE_GUID_REGISTRATION {
	guest_ptr<const GUID> Guid;
	HANDLE RegHandle;
};

static_assert(sizeof(TRACEGUID_HANDLE) == 8);
static_assert(sizeof(TRACE_GUID_REGISTRATION) == 2 * sizeof(GUEST_PTR));
static_assert(offsetof(TRACE_GUID_REGISTRATION, RegHandle) == sizeof(GUEST_PTR));

namespace advapi32 {

ULONG WINAPI RegisterTraceGuidsA(WMIDPREQUEST RequestAddress, PVOID RequestContext, const GUID *ControlGuid,
								 ULONG GuidCount, TRACE_GUID_REGISTRATION *TraceGuidReg, LPCSTR MofImagePath,
								 LPCSTR MofResourceName, TRACEGUID_HANDLE *RegistrationHandle);
ULONG WINAPI RegisterTraceGuidsW(WMIDPREQUEST RequestAddress, PVOID RequestContext, const GUID *ControlGuid,
								 ULONG GuidCount, TRACE_GUID_REGISTRATION *TraceGuidReg, LPCWSTR MofImagePath,
								 LPCWSTR MofResourceName, TRACEGUID_HANDLE *RegistrationHandle);
ULONG WINAPI UnregisterTraceGuids(TRACEGUID_HANDLE RegistrationHandle);

} // namespace advapi32
