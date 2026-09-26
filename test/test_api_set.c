#include <windows.h>

#include "test_assert.h"

typedef void(WINAPI *SetError)(DWORD);

int main(void) {
	const char *fault = getenv("WIBO_FIXTURE_API_SET_RESPONSE");
	if (fault) {
		SetLastError(77);
		TEST_CHECK(LoadLibraryA("api-ms-win-core-errorhandling-l1-1-0.dll") == NULL);
		TEST_CHECK_EQ(strcmp(fault, "failed") == 0 ? ERROR_MOD_NOT_FOUND : ERROR_INVALID_DATA, GetLastError());
		return 0;
	}
	const char *contracts[] = {"api-ms-win-core-errorhandling-l1-1-0.dll", "API-MS-WIN-CORE-SYSINFO-L1-2-0"};
	for (unsigned i = 0; i < 2; ++i) {
		HMODULE module = LoadLibraryA(contracts[i]);
		TEST_CHECK(module != NULL);
		TEST_CHECK(GetModuleHandleA(contracts[i]) == module);
		HMODULE again = LoadLibraryA(contracts[i]);
		TEST_CHECK(again == module);
		if (i == 0) {
			SetError setError = (SetError)GetProcAddress(module, "SetLastError");
			TEST_CHECK(setError != NULL);
			setError(0x20001234);
			TEST_CHECK_EQ(0x20001234, GetLastError());
		} else {
			TEST_CHECK(GetProcAddress(module, "GetTickCount64") != NULL);
		}
		TEST_CHECK(FreeLibrary(again));
		TEST_CHECK(FreeLibrary(module));
	}
	TEST_CHECK(LoadLibraryA("api-ms-win-absent-synthetic-l1-1-0.dll") == NULL);
	TEST_CHECK_EQ(ERROR_MOD_NOT_FOUND, GetLastError());
	return 0;
}
