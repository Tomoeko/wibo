#pragma once

#include "ntdll.h"

namespace kernel32 {
#ifdef WIBO_GUEST_64
BOOLEAN CDECL RtlInstallFunctionTableCallback(ULONGLONG tableIdentifier, ULONGLONG baseAddress, DWORD length,
											  PGET_RUNTIME_FUNCTION_CALLBACK callback, PVOID context,
											  LPCWSTR outOfProcessCallbackDll);
BOOLEAN CDECL RtlDeleteFunctionTable(RUNTIME_FUNCTION *functionTable);
#endif
} // namespace kernel32
