#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
	if (argc != 2 || (strcmp(argv[1], "native") && strcmp(argv[1], "wibo")))
		return 20;
	const DWORD expected = strcmp(argv[1], "native") ? 127 : 2;
	const DWORD selectors[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
	HANDLE original[3];
	for (unsigned index = 0; index < 3; ++index)
		original[index] = GetStdHandle(selectors[index]);
	char application[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, application, sizeof(application));
	if (!length || length >= sizeof(application))
		return 24;
	char *separator = strrchr(application, '\\');
	const char childName[] = "test_diagnostic_missing_child.exe";
	if (!separator || (size_t)(separator + 1 - application) + sizeof(childName) > sizeof(application))
		return 25;
	memcpy(separator + 1, childName, sizeof(childName));
	unsigned changed = 0;
	BOOL created = FALSE;
	PROCESS_INFORMATION process = {0};
	DWORD exitCode = 0;
	int good = 0;
	for (; changed < 3; ++changed)
		if (!SetStdHandle(selectors[changed], NULL))
			goto cleanup;
	STARTUPINFOA startup = {0};
	startup.cb = sizeof(startup);
	char command[] = "fixture";
	created = CreateProcessA(application, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
	if (!created || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0 ||
		!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode != expected)
		goto cleanup;
	good = 1;

cleanup:
	while (changed) {
		--changed;
		if (!SetStdHandle(selectors[changed], original[changed]))
			good = 0;
	}
	if (created && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process.hProcess, 90);
		DWORD reaped = WaitForSingleObject(process.hProcess, 5000);
		if (!terminated || reaped != WAIT_OBJECT_0)
			good = 0;
	}
	if (process.hThread && !CloseHandle(process.hThread))
		good = 0;
	if (process.hProcess && !CloseHandle(process.hProcess))
		good = 0;
	printf("diagnostic child: created=%d code=%lu expected=%lu result=%d\n", created, (unsigned long)exitCode,
		   (unsigned long)expected, good);
	return good ? 0 : 1;
}
