#pragma once

#include "minwinbase.h"
#include "types.h"

struct TIME_ZONE_INFORMATION {
	LONG Bias;
	WCHAR StandardName[32];
	SYSTEMTIME StandardDate;
	LONG StandardBias;
	WCHAR DaylightName[32];
	SYSTEMTIME DaylightDate;
	LONG DaylightBias;
};

static_assert(sizeof(TIME_ZONE_INFORMATION) == 172);

using LPTIME_ZONE_INFORMATION = TIME_ZONE_INFORMATION *;

struct DYNAMIC_TIME_ZONE_INFORMATION {
	LONG Bias;
	WCHAR StandardName[32];
	SYSTEMTIME StandardDate;
	LONG StandardBias;
	WCHAR DaylightName[32];
	SYSTEMTIME DaylightDate;
	LONG DaylightBias;
	WCHAR TimeZoneKeyName[128];
	BOOLEAN DynamicDaylightTimeDisabled;
};
static_assert(sizeof(DYNAMIC_TIME_ZONE_INFORMATION) == 432);
using PDYNAMIC_TIME_ZONE_INFORMATION = DYNAMIC_TIME_ZONE_INFORMATION *;

constexpr DWORD TIME_ZONE_ID_UNKNOWN = 0;
constexpr DWORD TIME_ZONE_ID_STANDARD = 1;
constexpr DWORD TIME_ZONE_ID_DAYLIGHT = 2;
constexpr DWORD TIME_ZONE_ID_INVALID = 0xFFFFFFFFu;

namespace kernel32 {

BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime);
BOOL WINAPI FileTimeToSystemTime(const FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime);
BOOL WINAPI SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *zone, const SYSTEMTIME *utc,
											LPSYSTEMTIME local);
BOOL WINAPI TzSpecificLocalTimeToSystemTime(const TIME_ZONE_INFORMATION *zone, const SYSTEMTIME *local,
											LPSYSTEMTIME utc);
BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *lpFileTime, LPFILETIME lpLocalFileTime);
BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *lpLocalFileTime, LPFILETIME lpFileTime);
BOOL WINAPI DosDateTimeToFileTime(WORD wFatDate, WORD wFatTime, LPFILETIME lpFileTime);
BOOL WINAPI FileTimeToDosDateTime(const FILETIME *lpFileTime, LPWORD lpFatDate, LPWORD lpFatTime);
DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION lpTimeZoneInformation);
DWORD WINAPI GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION information);

} // namespace kernel32
