#include "unwindapi.h"

namespace kernel32 {
#ifdef WIBO_GUEST_64
RUNTIME_FUNCTION *WINAPI RtlLookupFunctionEntry(ULONGLONG controlPc, ULONGLONG *imageBase, PVOID historyTable) {
	return ntdll::RtlLookupFunctionEntry(controlPc, imageBase, historyTable);
}

PVOID WINAPI RtlVirtualUnwind(DWORD handlerType, ULONGLONG imageBase, ULONGLONG controlPc,
							  RUNTIME_FUNCTION *functionEntry, CONTEXT64 *context, PVOID *handlerData, ULONGLONG *frame,
							  KNONVOLATILE_CONTEXT_POINTERS *pointers) {
	return ntdll::RtlVirtualUnwind(handlerType, imageBase, controlPc, functionEntry, context, handlerData, frame,
								   pointers);
}

BOOLEAN CDECL RtlInstallFunctionTableCallback(ULONGLONG tableIdentifier, ULONGLONG baseAddress, DWORD length,
											  PGET_RUNTIME_FUNCTION_CALLBACK callback, PVOID context,
											  LPCWSTR outOfProcessCallbackDll) {
	return ntdll::RtlInstallFunctionTableCallback(tableIdentifier, baseAddress, length, callback, context,
														  outOfProcessCallbackDll);
}

BOOLEAN CDECL RtlAddFunctionTable(RUNTIME_FUNCTION *functionTable, DWORD entryCount, ULONGLONG baseAddress) {
	return ntdll::RtlAddFunctionTable(functionTable, entryCount, baseAddress);
}

BOOLEAN CDECL RtlDeleteFunctionTable(RUNTIME_FUNCTION *functionTable) {
	return ntdll::RtlDeleteFunctionTable(functionTable);
}
#endif
} // namespace kernel32
