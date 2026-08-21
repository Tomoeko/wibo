// clang-format off: MinGW's dbghelp.h requires the Windows base types first.
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include "test_assert.h"

int main(void) {
	const DWORD options = SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS;
	TEST_CHECK_EQ(options, SymSetOptions(options));
	TEST_CHECK(SymInitialize((HANDLE)(ULONG_PTR)1, NULL, FALSE));

	HMODULE mainModule = GetModuleHandleA(NULL);
	DWORD64 address = (DWORD64)(ULONG_PTR)mainModule;
	TEST_CHECK_U64_EQ(address, SymGetModuleBase64((HANDLE)(ULONG_PTR)1, address));
	TEST_CHECK(SymFunctionTableAccess64((HANDLE)(ULONG_PTR)1, address) == NULL);
	return 0;
}
