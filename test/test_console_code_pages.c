#include <stdio.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

struct Query {
	UINT value;
	DWORD error;
};

struct Report {
	DWORD magic;
	DWORD standardType[3];
	DWORD initialNull[3];
	DWORD standardNull[3];
	struct Query query[4];
	DWORD result;
};

static const DWORD selectors[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

static int childMain(const WCHAR *reportPath) {
	struct Report report = {0};
	report.magic = 0x43504331;
	HANDLE output =
		CreateFileW(reportPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (output == INVALID_HANDLE_VALUE)
		return 20;
	for (unsigned index = 0; index < 3; ++index) {
		report.initialNull[index] = GetStdHandle(selectors[index]) == NULL;
		report.standardType[index] = GetFileType(GetStdHandle(selectors[index]));
	}
	for (unsigned index = 0; index < 4; ++index) {
		if (index == 2) {
			for (unsigned slot = 0; slot < 3; ++slot) {
				if (!SetStdHandle(selectors[slot], NULL)) {
					report.result = 21;
					goto complete;
				}
				report.standardNull[slot] = GetStdHandle(selectors[slot]) == NULL;
			}
		}
		SetLastError(0x4321);
		report.query[index].value = index % 2 ? GetConsoleOutputCP() : GetConsoleCP();
		report.query[index].error = GetLastError();
	}
complete:
	DWORD written = 0;
	if (!WriteFile(output, &report, sizeof(report), &written, NULL) || written != sizeof(report))
		report.result = 22;
	if (!CloseHandle(output))
		report.result = 23;
	return (int)report.result;
}

static int parentMain(int defaultStandards, int inheritDefault) {
	int result = 1;
	WCHAR temporary[MAX_PATH], reportPath[MAX_PATH] = {0}, executable[32768], command[32768];
	HANDLE pipes[4] = {NULL, NULL, NULL, NULL};
	HANDLE reader = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION process = {0};
	STARTUPINFOW startup = {0};
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
	DWORD length = GetModuleFileNameW(NULL, executable, sizeof(executable) / sizeof(*executable));
	DWORD temporaryLength = GetTempPathW(MAX_PATH, temporary);
	if (!length || length >= sizeof(executable) / sizeof(*executable) || !temporaryLength ||
		temporaryLength >= MAX_PATH || !GetTempFileNameW(temporary, L"cpc", 0, reportPath))
		goto cleanup;
	if (!defaultStandards &&
		(!CreatePipe(&pipes[0], &pipes[1], &attributes, 0) || !CreatePipe(&pipes[2], &pipes[3], &attributes, 0) ||
		 !SetHandleInformation(pipes[1], HANDLE_FLAG_INHERIT, 0) ||
		 !SetHandleInformation(pipes[2], HANDLE_FLAG_INHERIT, 0)))
		goto cleanup;
	int characters =
		swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" --child \"%ls\"", executable, reportPath);
	if (characters < 0 || (size_t)characters >= sizeof(command) / sizeof(*command))
		goto cleanup;
	startup.cb = sizeof(startup);
	startup.dwFlags = defaultStandards ? 0 : STARTF_USESTDHANDLES;
	startup.hStdInput = pipes[0];
	startup.hStdOutput = pipes[3];
	startup.hStdError = pipes[3];
	DWORD creationFlags = DETACHED_PROCESS;
	if (!CreateProcessW(executable, command, NULL, NULL, !defaultStandards || inheritDefault, creationFlags, NULL, NULL,
						&startup, &process))
		goto cleanup;
	if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
		goto cleanup;
	DWORD exitCode;
	if (!GetExitCodeProcess(process.hProcess, &exitCode) || exitCode)
		goto cleanup;
	reader = CreateFileW(reportPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (reader == INVALID_HANDLE_VALUE)
		goto cleanup;
	struct Report report = {0};
	DWORD read = 0;
	if (!ReadFile(reader, &report, sizeof(report), &read, NULL) || read != sizeof(report) ||
		report.magic != 0x43504331 || report.result)
		goto cleanup;
	for (unsigned slot = 0; slot < 3; ++slot) {
		if (report.standardType[slot] != (defaultStandards ? FILE_TYPE_UNKNOWN : FILE_TYPE_PIPE) ||
			report.initialNull[slot] != (DWORD)defaultStandards || !report.standardNull[slot])
			goto cleanup;
	}
	for (unsigned index = 0; index < 4; ++index) {
		if (report.query[index].value != 0 || report.query[index].error != ERROR_INVALID_HANDLE)
			goto cleanup;
	}
	result = 0;
cleanup:
	if (process.hProcess && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		if (!TerminateProcess(process.hProcess, 24) || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
			result = 2;
	}
	if (reader != INVALID_HANDLE_VALUE && !CloseHandle(reader))
		result = 3;
	if (process.hThread && !CloseHandle(process.hThread))
		result = 3;
	if (process.hProcess && !CloseHandle(process.hProcess))
		result = 3;
	for (unsigned index = 0; index < 4; ++index)
		if (pipes[index] && !CloseHandle(pipes[index]))
			result = 3;
	if (reportPath[0] && !DeleteFileW(reportPath))
		result = 3;
	return result;
}

int wmain(int argc, WCHAR **argv) {
	if (argc == 3 && wcscmp(argv[1], L"--child") == 0)
		return childMain(argv[2]);
	TEST_CHECK_EQ(1, argc);
	TEST_CHECK_EQ(0, parentMain(0, 0));
	TEST_CHECK_EQ(0, parentMain(1, 0));
	TEST_CHECK_EQ(0, parentMain(1, 1));
	return 0;
}
