#define _WIN32_WINNT 0x0601
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "child") == 0)
		return 7;
	TEST_CHECK_EQ(1, argc);
	char application[MAX_PATH];
	WCHAR wideApplication[MAX_PATH];
	const DWORD length = GetModuleFileNameA(NULL, application, MAX_PATH);
	const DWORD wideLength = GetModuleFileNameW(NULL, wideApplication, MAX_PATH);
	TEST_CHECK(length && length < MAX_PATH);
	TEST_CHECK(wideLength && wideLength < MAX_PATH);
	const DWORD sizes[] = {
		0, 1, sizeof(STARTUPINFOA) - 1, sizeof(STARTUPINFOA), sizeof(STARTUPINFOEXA), sizeof(STARTUPINFOEXA) + 16};
	for (unsigned api = 0; api < 2; ++api) {
		for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
			// The entire structure is valid storage even when cb describes another size.
			STARTUPINFOEXA startup;
			memset(&startup, 0, sizeof(startup));
			startup.StartupInfo.cb = sizes[index];
			startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
			startup.StartupInfo.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION process = {0};
			char command[] = "fixture child";
			WCHAR wideCommand[] = L"fixture child";
			SetLastError(0x4321);
			const BOOL created = api ? CreateProcessW(wideApplication, wideCommand, NULL, NULL, FALSE, CREATE_NO_WINDOW,
													  NULL, NULL, (LPSTARTUPINFOW)&startup, &process)
									 : CreateProcessA(application, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
													  NULL, &startup.StartupInfo, &process);
			const DWORD error = GetLastError();
			printf("api=%u cb=%lu created=%d error=%lu\n", api, (unsigned long)sizes[index], created,
				   (unsigned long)error);
			TEST_CHECK(created);
			TEST_CHECK_EQ(0x4321, error);
			TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 5000));
			DWORD exitCode = 0;
			TEST_CHECK(GetExitCodeProcess(process.hProcess, &exitCode));
			TEST_CHECK_EQ(7, exitCode);
			TEST_CHECK(CloseHandle(process.hThread));
			TEST_CHECK(CloseHandle(process.hProcess));
		}
	}
	return 0;
}
