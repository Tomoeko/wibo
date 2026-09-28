#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int runCase(DWORD flags, BOOL expectSuccess) {
	WCHAR executable[32768], command[32768];
	DWORD length = GetModuleFileNameW(NULL, executable, sizeof(executable) / sizeof(*executable));
	if (!length || length >= sizeof(executable) / sizeof(*executable))
		return 1;
	int count = swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" --child", executable);
	if (count <= 0 || (size_t)count >= sizeof(command) / sizeof(*command))
		return 2;
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	BOOL created = CreateProcessW(executable, command, NULL, NULL, FALSE, flags, NULL, NULL, &startup, &process);
	if (!created)
		return expectSuccess ? 3 : 0;
	int failed = !expectSuccess;
	if (expectSuccess && GetPriorityClass(process.hProcess) != NORMAL_PRIORITY_CLASS)
		failed = 1;
	DWORD waited = WaitForSingleObject(process.hProcess, 5000), exitCode = 0;
	if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &exitCode) || exitCode) {
		failed = 1;
		if (waited != WAIT_OBJECT_0)
			TerminateProcess(process.hProcess, 4);
	}
	if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
		failed = 1;
	return failed ? 4 : 0;
}

int wmain(int argc, WCHAR **argv) {
	if (argc == 2 && wcscmp(argv[1], L"--child") == 0)
		return 0;
	if (argc != 1)
		return 5;
	int normal = runCase(CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS, TRUE);
	int conflicting = getenv("WIBO_FIXTURE_RUNTIME")
						 ? runCase(CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS | IDLE_PRIORITY_CLASS, FALSE)
						 : 0;
	if (normal || conflicting)
		fprintf(stderr, "process priority fixture: normal=%d conflicting=%d\n", normal, conflicting);
	return normal ? normal : conflicting;
}
