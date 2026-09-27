#pragma once

#include "types.h"

using REGHANDLE = ULONGLONG;

enum EVENT_INFO_CLASS : ULONG {
	EventProviderBinaryTrackInfo = 0,
	EventProviderSetReserved1 = 1,
	EventProviderSetTraits = 2,
	EventProviderUseDescriptorType = 3,
	EventProviderSetReserved2 = 4,
	MaxEventInfo = 5,
};

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
ULONG WINAPI EventSetInformation(REGHANDLE RegHandle, EVENT_INFO_CLASS InformationClass, PVOID EventInformation,
								 ULONG InformationLength);

} // namespace advapi32
