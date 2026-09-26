#pragma once

#include "types.h"

constexpr DWORD TH32CS_SNAPPROCESS = 0x00000002;
constexpr DWORD TH32CS_INHERIT = 0x80000000;
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

template <class Character> struct ProcessEntry32 {
	DWORD dwSize;
	DWORD cntUsage;
	DWORD th32ProcessID;
	ULONG_PTR th32DefaultHeapID;
	DWORD th32ModuleID;
	DWORD cntThreads;
	DWORD th32ParentProcessID;
	LONG pcPriClassBase;
	DWORD dwFlags;
	Character szExeFile[260];
};
using PROCESSENTRY32 = ProcessEntry32<char>;
using PROCESSENTRY32W = ProcessEntry32<WCHAR>;
using LPPROCESSENTRY32 = PROCESSENTRY32 *;
using LPPROCESSENTRY32W = PROCESSENTRY32W *;

namespace kernel32 {

HANDLE WINAPI CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
BOOL WINAPI Process32First(HANDLE snapshot, LPPROCESSENTRY32 entry);
BOOL WINAPI Process32Next(HANDLE snapshot, LPPROCESSENTRY32 entry);
BOOL WINAPI Process32FirstW(HANDLE snapshot, LPPROCESSENTRY32W entry);
BOOL WINAPI Process32NextW(HANDLE snapshot, LPPROCESSENTRY32W entry);
BOOL WINAPI Module32FirstW(HANDLE hSnapshot, LPMODULEENTRY32W lpme);
BOOL WINAPI Module32NextW(HANDLE hSnapshot, LPMODULEENTRY32W lpme);

} // namespace kernel32
