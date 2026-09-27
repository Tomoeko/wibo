#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_assert.h"

struct Observation {
	DWORD magic;
	DWORD startupFlags;
	uint64_t handles[3];
	DWORD types[3];
	DWORD errors[3];
};

static const DWORD selectors[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE old = *handle;
	*handle = NULL;
	return CloseHandle(old) != FALSE;
}

static int childMain(const char *path) {
	struct Observation result = {0};
	result.magic = 0x53544432;
	STARTUPINFOA startup;
	memset(&startup, 0, sizeof(startup));
	GetStartupInfoA(&startup);
	result.startupFlags = startup.dwFlags;
	for (unsigned index = 0; index < 3; ++index) {
		HANDLE handle = GetStdHandle(selectors[index]);
		result.handles[index] = (uint64_t)(uintptr_t)handle;
		SetLastError(0x4321);
		result.types[index] = GetFileType(handle);
		result.errors[index] = GetLastError();
	}
	// Report through a separately opened file; never perform I/O on an invalid stream.
	HANDLE report = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
								FILE_ATTRIBUTE_NORMAL, NULL);
	if (report == INVALID_HANDLE_VALUE)
		return 21;
	DWORD written = 0;
	BOOL good = WriteFile(report, &result, sizeof(result), &written, NULL);
	BOOL closed = CloseHandle(report);
	return good && written == sizeof(result) && closed ? 0 : 22;
}

static int runCase(unsigned api, unsigned mode) {
	int good = 0;
	HANDLE input = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION process = {0};
	char directory[MAX_PATH], reportPath[MAX_PATH] = {0}, inputPath[MAX_PATH] = {0};
	char application[MAX_PATH], command[MAX_PATH * 2 + 32];
	WCHAR wideApplication[MAX_PATH], wideCommand[MAX_PATH * 2 + 32];
	const HANDLE original[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
								GetStdHandle(STD_ERROR_HANDLE)};
	unsigned standardsChanged = 0;
	const HANDLE absent = (HANDLE)(uintptr_t)0x7fff0004;
	BOOL created = FALSE;
	BOOL reportCreated = FALSE, inputCreated = FALSE;
	DWORD creationError = 0, exitCode = 0;
	struct Observation observation = {0};
	DWORD length = GetModuleFileNameA(NULL, application, MAX_PATH);
	if (!length || length >= MAX_PATH)
		goto cleanup;
	length = GetModuleFileNameW(NULL, wideApplication, MAX_PATH);
	if (!length || length >= MAX_PATH)
		goto cleanup;
	length = GetTempPathA(MAX_PATH, directory);
	if (!length || length >= MAX_PATH)
		goto cleanup;
	reportCreated = GetTempFileNameA(directory, "smr", 0, reportPath) != 0;
	if (!reportCreated)
		goto cleanup;
	inputCreated = GetTempFileNameA(directory, "smi", 0, inputPath) != 0;
	if (!inputCreated)
		goto cleanup;
	input = CreateFileA(inputPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL, NULL);
	if (input == INVALID_HANDLE_VALUE)
		goto cleanup;
	HANDLE chosen[3] = {input, absent, absent};
	if (mode == 1)
		chosen[0] = chosen[1] = chosen[2] = NULL;
	if (mode == 2)
		chosen[0] = chosen[1] = chosen[2] = INVALID_HANDLE_VALUE;
	if (mode == 3)
		chosen[0] = chosen[1] = chosen[2] = absent;
	DWORD flags = 0;
	if (GetHandleInformation(absent, &flags) || GetLastError() != ERROR_INVALID_HANDLE)
		goto cleanup;
	for (; standardsChanged < 3; ++standardsChanged)
		if (!SetStdHandle(selectors[standardsChanged], chosen[standardsChanged]))
			goto cleanup;
	STARTUPINFOEXA startup;
	memset(&startup, 0, sizeof(startup));
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
	startup.StartupInfo.wShowWindow = SW_HIDE;
	// These members are ignored because STARTF_USESTDHANDLES is absent.
	startup.StartupInfo.hStdInput = (HANDLE)(uintptr_t)0x7fff0001;
	startup.StartupInfo.hStdOutput = (HANDLE)(uintptr_t)0x7fff0002;
	startup.StartupInfo.hStdError = (HANDLE)(uintptr_t)0x7fff0003;
	int count = snprintf(command, sizeof(command), "fixture child \"%s\"", reportPath);
	if (count <= 0 || (size_t)count >= sizeof(command))
		goto cleanup;
	for (int index = 0; index <= count; ++index)
		wideCommand[index] = (unsigned char)command[index];
	SetLastError(0x4321);
	created = api ? CreateProcessW(wideApplication, wideCommand, NULL, NULL, mode == 3, 0, NULL, NULL,
								   (LPSTARTUPINFOW)&startup, &process)
				  : CreateProcessA(application, command, NULL, NULL, mode == 3, 0, NULL, NULL, &startup.StartupInfo,
								   &process);
	creationError = GetLastError();
	if (!created || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0 ||
		!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode)
		goto cleanup;
	HANDLE report = CreateFileA(reportPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
								FILE_ATTRIBUTE_NORMAL, NULL);
	if (report == INVALID_HANDLE_VALUE)
		goto cleanup;
	DWORD read = 0;
	BOOL readResult = ReadFile(report, &observation, sizeof(observation), &read, NULL);
	BOOL closed = CloseHandle(report);
	if (!readResult || read != sizeof(observation) || !closed || observation.magic != 0x53544432 ||
		(observation.startupFlags & STARTF_USESTDHANDLES) || creationError != 0x4321)
		goto cleanup;
	for (unsigned index = 0; index < 3; ++index) {
		if (mode == 0 && index == 0) {
			if (!observation.handles[index] || observation.handles[index] == (uint64_t)(uintptr_t)input ||
				observation.types[index] != FILE_TYPE_DISK || observation.errors[index] != 0x4321)
				goto cleanup;
		} else if (observation.handles[index] != (mode == 3 ? (uint64_t)(uintptr_t)absent : 0) ||
				   observation.types[index] != FILE_TYPE_UNKNOWN || observation.errors[index] != ERROR_INVALID_HANDLE) {
			goto cleanup;
		}
	}
	good = 1;

cleanup:
	while (standardsChanged) {
		--standardsChanged;
		if (!SetStdHandle(selectors[standardsChanged], original[standardsChanged]))
			good = 0;
	}
	if (created && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process.hProcess, 90);
		DWORD reaped = WaitForSingleObject(process.hProcess, 5000);
		fprintf(stderr, "cleanup pid=%lu terminated=%d reaped=%lu\n", (unsigned long)process.dwProcessId, terminated,
				(unsigned long)reaped);
		if (!terminated || reaped != WAIT_OBJECT_0)
			good = 0;
	}
	if (!closeOwned(&process.hThread))
		good = 0;
	if (!closeOwned(&process.hProcess))
		good = 0;
	if (!closeOwned(&input))
		good = 0;
	if (inputCreated && !DeleteFileA(inputPath))
		good = 0;
	if (reportCreated && !DeleteFileA(reportPath))
		good = 0;
	printf("api=%u mode=%u created=%d error=%lu code=%lu result=%d\n", api, mode, created, (unsigned long)creationError,
		   (unsigned long)exitCode, good);
	return good;
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0)
		return childMain(argv[2]);
	TEST_CHECK_EQ(1, argc);
	// This fixture is a console image; GUI default-stream policy is a separate scope.
	for (unsigned api = 0; api < 2; ++api)
		for (unsigned mode = 0; mode < 4; ++mode)
			TEST_CHECK(runCase(api, mode));
	return 0;
}
