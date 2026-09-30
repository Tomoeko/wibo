#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC address = GetProcAddress(kernel, "GetEnabledXStateFeatures");
	TEST_CHECK(address != NULL);
	DWORD64(WINAPI * query)(void);
	memcpy(&query, &address, sizeof(query));

	SetLastError(77);
	DWORD64 importedMask = GetEnabledXStateFeatures();
	TEST_CHECK_EQ(77, GetLastError());
	SetLastError(83);
	DWORD64 dynamicMask = query();
	TEST_CHECK_EQ(83, GetLastError());
	TEST_CHECK_U64_EQ(importedMask, dynamicMask);
	TEST_CHECK((importedMask & XSTATE_MASK_LEGACY) == 0 || (importedMask & XSTATE_MASK_LEGACY) == XSTATE_MASK_LEGACY);
	return 0;
}
