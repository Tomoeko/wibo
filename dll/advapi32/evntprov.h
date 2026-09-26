#pragma once

#include "types.h"

using REGHANDLE = ULONGLONG;

struct EVENT_FILTER_DESCRIPTOR {
	ULONGLONG Ptr;
	ULONG Size;
	ULONG Type;
};

typedef VOID(_CC_STDCALL *PENABLECALLBACK)(const GUID *SourceId, ULONG IsEnabled, UCHAR Level,
										   ULONGLONG MatchAnyKeyword, ULONGLONG MatchAllKeyword,
										   EVENT_FILTER_DESCRIPTOR *FilterData, PVOID CallbackContext);

namespace advapi32 {

ULONG WINAPI EventRegister(const GUID *ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext,
						   REGHANDLE *RegHandle);
ULONG WINAPI EventUnregister(REGHANDLE RegHandle);

} // namespace advapi32
