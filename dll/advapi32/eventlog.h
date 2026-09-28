#pragma once

#include "types.h"

namespace advapi32 {

HANDLE WINAPI RegisterEventSourceW(LPCWSTR serverName, LPCWSTR sourceName);
BOOL WINAPI ReportEventW(HANDLE eventLog, WORD type, WORD category, DWORD eventId, PSID userSid, WORD stringCount,
						 DWORD dataSize, const GUEST_PTR *strings, LPVOID rawData);
BOOL WINAPI DeregisterEventSource(HANDLE eventLog);

} // namespace advapi32
