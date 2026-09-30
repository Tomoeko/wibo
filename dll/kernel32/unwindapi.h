#pragma once

#include "ntdll.h"

namespace kernel32 {
#ifdef WIBO_GUEST_64
void WINAPI RtlCaptureContext(CONTEXT64 *context) WIBO_ANNOTATE("GUEST_ENTRY:wiboCaptureContext64");
RUNTIME_FUNCTION *WINAPI RtlLookupFunctionEntry(ULONGLONG controlPc, ULONGLONG *imageBase, PVOID historyTable);
PVOID WINAPI RtlVirtualUnwind(DWORD handlerType, ULONGLONG imageBase, ULONGLONG controlPc,
							  RUNTIME_FUNCTION *functionEntry, CONTEXT64 *context, PVOID *handlerData, ULONGLONG *frame,
							  KNONVOLATILE_CONTEXT_POINTERS *pointers);
BOOLEAN CDECL RtlInstallFunctionTableCallback(ULONGLONG tableIdentifier, ULONGLONG baseAddress, DWORD length,
											  PGET_RUNTIME_FUNCTION_CALLBACK callback, PVOID context,
											  LPCWSTR outOfProcessCallbackDll);
BOOLEAN CDECL RtlAddFunctionTable(RUNTIME_FUNCTION *functionTable, DWORD entryCount, ULONGLONG baseAddress);
BOOLEAN CDECL RtlDeleteFunctionTable(RUNTIME_FUNCTION *functionTable);
#endif
} // namespace kernel32
