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
	address = GetProcAddress(kernel, "K32GetModuleBaseNameA");
	TEST_CHECK(address != NULL);
	DWORD(WINAPI * baseNameAlias)(HANDLE, HMODULE, LPSTR, DWORD);
	memcpy(&baseNameAlias, &address, sizeof(baseNameAlias));
	address = GetProcAddress(kernel, "K32GetModuleBaseNameW");
	TEST_CHECK(address != NULL);
	DWORD(WINAPI * baseNameWideAlias)(HANDLE, HMODULE, LPWSTR, DWORD);
	memcpy(&baseNameWideAlias, &address, sizeof(baseNameWideAlias));
	address = GetProcAddress(kernel, "K32GetModuleFileNameExA");
	TEST_CHECK(address != NULL);
	DWORD(WINAPI * fileNameAlias)(HANDLE, HMODULE, LPSTR, DWORD);
	memcpy(&fileNameAlias, &address, sizeof(fileNameAlias));
	address = GetProcAddress(kernel, "K32GetModuleFileNameExW");
	TEST_CHECK(address != NULL);
	DWORD(WINAPI * fileNameWideAlias)(HANDLE, HMODULE, LPWSTR, DWORD);
	memcpy(&fileNameWideAlias, &address, sizeof(fileNameWideAlias));
	address = GetProcAddress(kernel, "K32GetModuleInformation");
	TEST_CHECK(address != NULL);
	BOOL(WINAPI * moduleInfoAlias)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
	memcpy(&moduleInfoAlias, &address, sizeof(moduleInfoAlias));
	address = GetProcAddress(kernel, "K32GetProcessMemoryInfo");
	TEST_CHECK(address != NULL);
	BOOL(WINAPI * memoryInfoAlias)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
	memcpy(&memoryInfoAlias, &address, sizeof(memoryInfoAlias));
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
	FARPROC timeZone = GetProcAddress(kernel, "GetTimeZoneInformation");
	TEST_CHECK(timeZone != NULL);
	BOOL foundMain = FALSE, foundKernel = FALSE;
	for (DWORD i = 0; i < required / sizeof(HMODULE); ++i) {
		if (modules[i] == mainModule) {
			foundMain = TRUE;
		}
		if (modules[i] == kernel) {
			foundKernel = TRUE;
			TEST_CHECK(GetProcAddress(modules[i], "GetTimeZoneInformation") == timeZone);
		}
	}
	TEST_CHECK(foundMain);
	TEST_CHECK(foundKernel);

	char baseName[MAX_PATH];
	TEST_CHECK(GetModuleBaseNameA(process, mainModule, baseName, sizeof(baseName)) > 0);
	TEST_CHECK(strstr(baseName, "test_psapi") != NULL);

	TEST_CHECK(GetModuleBaseNameA(process, kernel, baseName, sizeof(baseName)) > 0);
	TEST_CHECK(_stricmp(baseName, "kernel32.dll") == 0);
	TEST_CHECK(baseNameAlias(process, kernel, baseName, sizeof(baseName)) > 0);
	TEST_CHECK(_stricmp(baseName, "kernel32.dll") == 0);

	WCHAR wideName[MAX_PATH];
	WCHAR aliasWideName[MAX_PATH];
	DWORD wideLength = GetModuleBaseNameW(process, mainModule, wideName, MAX_PATH);
	TEST_CHECK(wideLength > 0);
	TEST_CHECK(wcsstr(wideName, L"test_psapi") != NULL);
	TEST_CHECK_EQ(wideLength, baseNameWideAlias(process, mainModule, aliasWideName, MAX_PATH));
	TEST_CHECK(wcscmp(wideName, aliasWideName) == 0);
	TEST_CHECK_EQ(wideLength, baseNameWideAlias(process, NULL, aliasWideName, MAX_PATH));
	TEST_CHECK(wcscmp(wideName, aliasWideName) == 0);

	WCHAR expectedPath[MAX_PATH];
	DWORD pathLength = GetModuleFileNameW(mainModule, expectedPath, MAX_PATH);
	TEST_CHECK(pathLength > 0);
	TEST_CHECK_EQ(pathLength, fileNameWideAlias(process, mainModule, aliasWideName, MAX_PATH));
	TEST_CHECK(wcscmp(expectedPath, aliasWideName) == 0);
	TEST_CHECK_EQ(pathLength, fileNameWideAlias(process, NULL, aliasWideName, MAX_PATH));
	TEST_CHECK(wcscmp(expectedPath, aliasWideName) == 0);
	WCHAR shortPath[4];
	TEST_CHECK_EQ(4, fileNameWideAlias(process, mainModule, shortPath, 4));
	TEST_CHECK_EQ(0, shortPath[3]);
	char expectedPathA[MAX_PATH];
	char aliasPathA[MAX_PATH];
	DWORD pathLengthA = GetModuleFileNameA(mainModule, expectedPathA, MAX_PATH);
	TEST_CHECK(pathLengthA > 0);
	TEST_CHECK_EQ(pathLengthA, fileNameAlias(process, mainModule, aliasPathA, MAX_PATH));
	TEST_CHECK(strcmp(expectedPathA, aliasPathA) == 0);

	MODULEINFO info;
	TEST_CHECK(GetModuleInformation(process, mainModule, &info, sizeof(info)));
	TEST_CHECK(info.lpBaseOfDll == (LPVOID)mainModule);
	TEST_CHECK(info.SizeOfImage > 0);
	TEST_CHECK(info.EntryPoint != NULL);
	MODULEINFO aliasInfo;
	TEST_CHECK(moduleInfoAlias(process, mainModule, &aliasInfo, sizeof(aliasInfo)));
	TEST_CHECK(aliasInfo.lpBaseOfDll == info.lpBaseOfDll);
	TEST_CHECK_EQ(info.SizeOfImage, aliasInfo.SizeOfImage);
	TEST_CHECK(aliasInfo.EntryPoint == info.EntryPoint);

	HANDLE opened = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
	TEST_CHECK(opened != NULL);
	DWORD openedRequired = 0;
	TEST_CHECK(EnumProcessModules(opened, NULL, 0, &openedRequired));
	TEST_CHECK_EQ(required, openedRequired);
	TEST_CHECK(GetModuleBaseNameA(opened, mainModule, baseName, sizeof(baseName)) > 0);
	TEST_CHECK(GetModuleInformation(opened, mainModule, &info, sizeof(info)));
	TEST_CHECK_EQ(pathLength, fileNameWideAlias(opened, mainModule, aliasWideName, MAX_PATH));
	TEST_CHECK(CloseHandle(opened));

	HANDLE limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(limited != NULL);
	SetLastError(ERROR_SUCCESS);
	TEST_CHECK(!EnumProcessModules(limited, NULL, 0, &openedRequired));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(pathLength, fileNameWideAlias(limited, NULL, aliasWideName, MAX_PATH));
	TEST_CHECK(CloseHandle(limited));

	PROCESS_MEMORY_COUNTERS counters;
	memset(&counters, 0xa5, sizeof(counters));
	TEST_CHECK(GetProcessMemoryInfo(process, &counters, sizeof(counters)));
	TEST_CHECK_EQ(sizeof(counters), counters.cb);
	TEST_CHECK(counters.WorkingSetSize > 0);
	TEST_CHECK(counters.PeakWorkingSetSize >= counters.WorkingSetSize);
	TEST_CHECK(memoryInfoAlias(process, &counters, sizeof(counters)));
	TEST_CHECK_EQ(sizeof(counters), counters.cb);

	limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(limited != NULL);
	TEST_CHECK(GetProcessMemoryInfo(limited, &counters, sizeof(counters)));
	TEST_CHECK(CloseHandle(limited));

	PROCESS_MEMORY_COUNTERS_EX extended;
	memset(&extended, 0xa5, sizeof(extended));
	SetLastError(ERROR_SUCCESS);
	if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&extended, sizeof(extended))) {
		TEST_CHECK(extended.cb == sizeof(counters) || extended.cb == sizeof(extended));
		TEST_CHECK(extended.PrivateUsage > 0);
	} else {
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK_EQ(0xa5a5a5a5, extended.cb);
	}
	TEST_CHECK(!GetProcessMemoryInfo(process, &counters, sizeof(counters) - 1));
	TEST_CHECK(!GetProcessMemoryInfo(NULL, &counters, sizeof(counters)));
	return 0;
}
