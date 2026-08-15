#pragma once

#include "types.h"

constexpr DWORD TH32CS_SNAPMODULE = 0x00000008;
constexpr DWORD TH32CS_SNAPMODULE32 = 0x00000010;

struct MODULEENTRY32W {
	DWORD dwSize;
	DWORD th32ModuleID;
	DWORD th32ProcessID;
	DWORD GlblcntUsage;
	DWORD ProccntUsage;
	GUEST_PTR modBaseAddr;
	DWORD modBaseSize;
	HMODULE hModule;
	WCHAR szModule[256];
	WCHAR szExePath[260];
};
using LPMODULEENTRY32W = MODULEENTRY32W *;

namespace kernel32 {

HANDLE WINAPI CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
BOOL WINAPI Module32FirstW(HANDLE hSnapshot, LPMODULEENTRY32W lpme);
BOOL WINAPI Module32NextW(HANDLE hSnapshot, LPMODULEENTRY32W lpme);

} // namespace kernel32
