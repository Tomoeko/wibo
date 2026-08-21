#pragma once

#include "types.h"

struct MODULEINFO {
	GUEST_PTR lpBaseOfDll;
	DWORD SizeOfImage;
	GUEST_PTR EntryPoint;
};

using LPMODULEINFO = MODULEINFO *;

namespace psapi {

BOOL WINAPI EnumProcessModules(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded);
DWORD WINAPI GetModuleBaseNameA(HANDLE hProcess, HMODULE hModule, LPSTR lpBaseName, DWORD nSize);
BOOL WINAPI GetModuleInformation(HANDLE hProcess, HMODULE hModule, LPMODULEINFO lpmodinfo, DWORD cb);

} // namespace psapi
