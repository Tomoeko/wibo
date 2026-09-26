#define PSAPI_VERSION 1
// clang-format off: MinGW's psapi.h requires the Windows base types first.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include "test_assert.h"

int main(void) {
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC address = GetProcAddress(kernel, "K32EnumProcessModules");
	TEST_CHECK(address != NULL);
	BOOL(WINAPI * enumerate)(HANDLE, HMODULE *, DWORD, LPDWORD);
	memcpy(&enumerate, &address, sizeof(enumerate));
	HANDLE process = GetCurrentProcess();
	DWORD required = 0;
	TEST_CHECK(EnumProcessModules(process, NULL, 0, &required));
	TEST_CHECK(required >= sizeof(HMODULE));

	HMODULE modules[64];
	TEST_CHECK(EnumProcessModules(process, modules, sizeof(modules), &required));
	TEST_CHECK(required <= sizeof(modules));

	HMODULE aliasModules[64];
	DWORD aliasRequired = 0;
	TEST_CHECK(enumerate(process, aliasModules, sizeof(aliasModules), &aliasRequired));
	TEST_CHECK_EQ(required, aliasRequired);
	TEST_CHECK(memcmp(modules, aliasModules, required) == 0);
	HMODULE truncated[2] = {NULL, (HMODULE)(ULONG_PTR)0x1234};
	TEST_CHECK(enumerate(process, truncated, sizeof(HMODULE), &aliasRequired));
	TEST_CHECK_EQ(required, aliasRequired);
	TEST_CHECK(truncated[0] == modules[0]);
	TEST_CHECK(truncated[1] == (HMODULE)(ULONG_PTR)0x1234);
	HMODULE mainModule = GetModuleHandleA(NULL);
	BOOL foundMain = FALSE;
	for (DWORD i = 0; i < required / sizeof(HMODULE); ++i) {
		if (modules[i] == mainModule) {
			foundMain = TRUE;
			break;
		}
	}
	TEST_CHECK(foundMain);

	char baseName[MAX_PATH];
	TEST_CHECK(GetModuleBaseNameA(process, mainModule, baseName, sizeof(baseName)) > 0);
	TEST_CHECK(strstr(baseName, "test_psapi") != NULL);

	MODULEINFO info;
	TEST_CHECK(GetModuleInformation(process, mainModule, &info, sizeof(info)));
	TEST_CHECK(info.lpBaseOfDll == (LPVOID)mainModule);
	TEST_CHECK(info.SizeOfImage > 0);
	TEST_CHECK(info.EntryPoint != NULL);
	return 0;
}
