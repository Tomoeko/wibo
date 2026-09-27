#include "unwindapi.h"

namespace kernel32 {
#ifdef WIBO_GUEST_64
BOOLEAN CDECL RtlInstallFunctionTableCallback(ULONGLONG tableIdentifier, ULONGLONG baseAddress, DWORD length,
											  PGET_RUNTIME_FUNCTION_CALLBACK callback, PVOID context,
											  LPCWSTR outOfProcessCallbackDll) {
	return ntdll::RtlInstallFunctionTableCallback(tableIdentifier, baseAddress, length, callback, context,
												  outOfProcessCallbackDll);
}

BOOLEAN CDECL RtlDeleteFunctionTable(RUNTIME_FUNCTION *functionTable) {
	return ntdll::RtlDeleteFunctionTable(functionTable);
}
#endif
} // namespace kernel32
