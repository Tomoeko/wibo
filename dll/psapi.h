#pragma once

#include "types.h"

struct MODULEINFO {
	GUEST_PTR lpBaseOfDll;
	DWORD SizeOfImage;
	GUEST_PTR EntryPoint;
};

using LPMODULEINFO = MODULEINFO *;

struct PROCESS_MEMORY_COUNTERS {
	DWORD cb;
	DWORD PageFaultCount;
	SIZE_T PeakWorkingSetSize;
	SIZE_T WorkingSetSize;
	SIZE_T QuotaPeakPagedPoolUsage;
	SIZE_T QuotaPagedPoolUsage;
	SIZE_T QuotaPeakNonPagedPoolUsage;
	SIZE_T QuotaNonPagedPoolUsage;
	SIZE_T PagefileUsage;
	SIZE_T PeakPagefileUsage;
};

struct PROCESS_MEMORY_COUNTERS_EX : PROCESS_MEMORY_COUNTERS {
	SIZE_T PrivateUsage;
};

using PPROCESS_MEMORY_COUNTERS = PROCESS_MEMORY_COUNTERS *;

namespace psapi {

BOOL WINAPI EnumProcessModules(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded);
DWORD WINAPI GetModuleBaseNameA(HANDLE hProcess, HMODULE hModule, LPSTR lpBaseName, DWORD nSize);
BOOL WINAPI GetModuleInformation(HANDLE hProcess, HMODULE hModule, LPMODULEINFO lpmodinfo, DWORD cb);
BOOL WINAPI GetProcessMemoryInfo(HANDLE process, PPROCESS_MEMORY_COUNTERS counters, DWORD cb);

} // namespace psapi
