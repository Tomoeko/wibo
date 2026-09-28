#pragma once

#include "minwinbase.h"
#include "types.h"

namespace kernel32 {
HANDLE WINAPI CreateJobObjectA(LPSECURITY_ATTRIBUTES attributes, LPCSTR name);
HANDLE WINAPI CreateJobObjectW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name);
BOOL WINAPI AssignProcessToJobObject(HANDLE job, HANDLE process);
BOOL WINAPI SetInformationJobObject(HANDLE job, DWORD informationClass, LPVOID information, DWORD length);
BOOL WINAPI QueryInformationJobObject(HANDLE job, DWORD informationClass, LPVOID information, DWORD length,
									  LPDWORD returnLength);
BOOL WINAPI IsProcessInJob(HANDLE process, HANDLE job, BOOL *result);
}
