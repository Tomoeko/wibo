#include <stdio.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

struct Observation {
	BOOL value;
	DWORD error;
};

struct Report {
	DWORD magic;
	struct Observation calls[15];
	DWORD invoked;
};

static LONG invoked;

static BOOL WINAPI handlerA(DWORD event) {
	(void)event;
	InterlockedIncrement(&invoked);
	return TRUE;
}

static BOOL WINAPI handlerB(DWORD event) {
	(void)event;
	InterlockedIncrement(&invoked);
	return FALSE;
}

static struct Observation observe(PHANDLER_ROUTINE handler, BOOL add) {
	struct Observation observation;
	SetLastError(0x4321);
	observation.value = SetConsoleCtrlHandler(handler, add);
	observation.error = GetLastError();
	return observation;
}

static void capture(struct Report *report) {
	report->magic = 0x43434831;
	report->calls[0] = observe(handlerA, FALSE);
	report->calls[1] = observe(NULL, FALSE);
	report->calls[2] = observe(NULL, TRUE);
	report->calls[3] = observe(NULL, -7);
	report->calls[4] = observe(NULL, FALSE);
	report->calls[5] = observe(handlerA, TRUE);
	report->calls[6] = observe(handlerA, -7);
	report->calls[7] = observe(handlerB, 2);
	report->calls[8] = observe(handlerA, FALSE);
	report->calls[9] = observe(handlerB, FALSE);
	report->calls[10] = observe(handlerA, FALSE);
	report->calls[11] = observe(handlerA, FALSE);
	report->calls[12] = observe(handlerB, FALSE);
	report->calls[13] = observe(NULL, FALSE);
	report->calls[14] = observe(NULL, FALSE);
	report->invoked = (DWORD)invoked;
}

static int checkReport(const struct Report *report) {
	if (report->magic != 0x43434831 || report->invoked)
		return 1;
	for (unsigned index = 0; index < 15; ++index) {
		const BOOL missing = index == 0 || index == 11 || index == 12;
		if (report->calls[index].value != !missing ||
			report->calls[index].error != (missing ? ERROR_INVALID_PARAMETER : 0x4321))
			return 2;
	}
	return 0;
}

static DWORD WINAPI registerFromThread(void *argument) {
	struct Observation *observation = argument;
	*observation = observe(handlerB, TRUE);
	return 0;
}

static int checkSharedRegistration(void) {
	struct Observation added = {0};
	HANDLE thread = CreateThread(NULL, 0, registerFromThread, &added, 0, NULL);
	if (!thread)
		return 1;
	if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
		ExitProcess(2);
	BOOL closed = CloseHandle(thread);
	struct Observation removed = observe(handlerB, FALSE);
	return closed && added.value == TRUE && added.error == 0x4321 && removed.value == TRUE && removed.error == 0x4321 &&
				   !invoked
			   ? 0
			   : 3;
}

static int childMain(const WCHAR *path) {
	struct Report report = {0};
	HANDLE output = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (output == INVALID_HANDLE_VALUE)
		return 20;
	capture(&report);
	DWORD written = 0;
	const BOOL stored = WriteFile(output, &report, sizeof(report), &written, NULL);
	const BOOL closed = CloseHandle(output);
	return stored && written == sizeof(report) && closed ? 0 : 21;
}

static int checkDetachedRegistration(void) {
	WCHAR temporary[MAX_PATH], path[MAX_PATH] = {0}, executable[32768], command[32768];
	PROCESS_INFORMATION process = {0};
	STARTUPINFOW startup = {0};
	HANDLE reader = INVALID_HANDLE_VALUE;
	BOOL parentRegistered = FALSE;
	int result = 1;
	DWORD length = GetModuleFileNameW(NULL, executable, sizeof(executable) / sizeof(*executable));
	DWORD temporaryLength = GetTempPathW(MAX_PATH, temporary);
	if (!length || length >= sizeof(executable) / sizeof(*executable) || !temporaryLength ||
		temporaryLength >= MAX_PATH || !GetTempFileNameW(temporary, L"cch", 0, path))
		goto cleanup;
	int characters =
		swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" --child \"%ls\"", executable, path);
	if (characters < 0 || (size_t)characters >= sizeof(command) / sizeof(*command))
		goto cleanup;
	struct Observation added = observe(handlerA, TRUE);
	parentRegistered = added.value != FALSE;
	if (!parentRegistered || added.error != 0x4321)
		goto cleanup;
	startup.cb = sizeof(startup);
	if (!CreateProcessW(executable, command, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &startup, &process))
		goto cleanup;
	if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
		goto cleanup;
	DWORD exitCode = 0;
	if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode)
		goto cleanup;
	reader = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (reader == INVALID_HANDLE_VALUE)
		goto cleanup;
	struct Report report = {0};
	DWORD read = 0;
	if (!ReadFile(reader, &report, sizeof(report), &read, NULL) || read != sizeof(report) || checkReport(&report))
		goto cleanup;
	result = 0;
cleanup:
	if (process.hProcess && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		if (!TerminateProcess(process.hProcess, 24) || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
			result = 3;
	}
	if (reader != INVALID_HANDLE_VALUE && !CloseHandle(reader))
		result = 4;
	if (process.hThread && !CloseHandle(process.hThread))
		result = 4;
	if (process.hProcess && !CloseHandle(process.hProcess))
		result = 4;
	if (path[0] && !DeleteFileW(path))
		result = 4;
	if (parentRegistered) {
		struct Observation removed = observe(handlerA, FALSE);
		if (!removed.value || removed.error != 0x4321)
			result = 5;
	}
	return result;
}

int wmain(int argc, WCHAR **argv) {
	if (argc == 3 && wcscmp(argv[1], L"--child") == 0)
		return childMain(argv[2]);
	TEST_CHECK_EQ(1, argc);
	struct Report report = {0};
	capture(&report);
	TEST_CHECK_EQ(0, checkReport(&report));
	TEST_CHECK_EQ(0, checkSharedRegistration());
	TEST_CHECK_EQ(0, checkDetachedRegistration());
	printf("registration_cases=30 callbacks=%ld\n", (long)invoked);
	return 0;
}
