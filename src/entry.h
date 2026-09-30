#pragma once

#include "types.h"

typedef VOID(_CC_CDECL *EntryProc)();
typedef int(_CC_CDECL *PNTDLL_FORMATTER)(char *buffer, SIZE_T count, const char *format, PVOID arguments);
typedef BOOL(_CC_STDCALL *DllEntryProc)(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved);
typedef VOID(_CC_STDCALL *PIMAGE_TLS_CALLBACK)(PVOID DllHandle, DWORD Reason, PVOID Reserved);

namespace entry {

void CDECL stubBase(SIZE_T index);

#ifndef WIBO_GUEST_64
DWORD CDECL prepareSoftwareException32(GUEST_PTR frame, DWORD mode);
DWORD CDECL finishSoftwareExceptionHandler32(GUEST_PTR frame, LONG disposition);
void CDECL abortSoftwareException32(DWORD code);
#endif

} // namespace entry
