#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef intptr_t(__cdecl *GetOsHandle)(int);
typedef int(__cdecl *CloseDescriptor)(int);
typedef int(__cdecl *OpenDescriptor)(const char *, int, ...);
typedef int *(__cdecl *GetCrtError)(void);
typedef void(__cdecl *InvalidParameterHandler)(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t);
typedef InvalidParameterHandler(__cdecl *SetInvalidParameterHandler)(InvalidParameterHandler);

struct DescriptorObservation {
	uint64_t before;
	uint64_t after;
	DWORD beforeType;
	DWORD afterType;
	int closeResult;
	int closeError;
	int openResult;
	int openError;
};

struct RepairReport {
	DWORD magic;
	DWORD result;
	DWORD completed;
	DWORD invalidParameters;
	DWORD standardNull[3];
	struct DescriptorObservation descriptor[3];
};

static unsigned invalidParameters;
static const DWORD selectors[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

static void __cdecl invalidParameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file,
									 unsigned line, uintptr_t reserved) {
	(void)expression;
	(void)function;
	(void)file;
	(void)line;
	(void)reserved;
	++invalidParameters;
}

static int childMain(const WCHAR *reportPath) {
	struct RepairReport report = {0};
	report.magic = 0x43525431;
	report.result = 20;
	// WinAPI handles do not allocate CRT descriptor slots.
	HANDLE output =
		CreateFileW(reportPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (output == INVALID_HANDLE_VALUE)
		return 21;
	HMODULE runtime = NULL;
	CloseDescriptor closeDescriptor = NULL;
	SetInvalidParameterHandler setHandler = NULL;
	InvalidParameterHandler originalHandler = NULL;
	int installedHandler = 0;
	int owned[3] = {-1, -1, -1};
	for (unsigned index = 0; index < 3; ++index) {
		report.standardNull[index] = GetStdHandle(selectors[index]) == NULL;
		if (!report.standardNull[index])
			goto cleanup;
	}
	runtime = LoadLibraryW(L"ucrtbase.dll");
	if (!runtime) {
		report.result = 22;
		goto cleanup;
	}
	GetOsHandle getOsHandle;
	OpenDescriptor openDescriptor;
	GetCrtError getCrtError;
	FARPROC getEntry = GetProcAddress(runtime, "_get_osfhandle");
	FARPROC closeEntry = GetProcAddress(runtime, "_close");
	FARPROC openEntry = GetProcAddress(runtime, "_open");
	FARPROC errorEntry = GetProcAddress(runtime, "_errno");
	FARPROC handlerEntry = GetProcAddress(runtime, "_set_invalid_parameter_handler");
	if (!getEntry || !closeEntry || !openEntry || !errorEntry || !handlerEntry) {
		report.result = 23;
		goto cleanup;
	}
	memcpy(&getOsHandle, &getEntry, sizeof(getOsHandle));
	memcpy(&closeDescriptor, &closeEntry, sizeof(closeDescriptor));
	memcpy(&openDescriptor, &openEntry, sizeof(openDescriptor));
	memcpy(&getCrtError, &errorEntry, sizeof(getCrtError));
	memcpy(&setHandler, &handlerEntry, sizeof(setHandler));
	originalHandler = setHandler(invalidParameter);
	installedHandler = 1;
	for (int descriptor = 0; descriptor < 3; ++descriptor) {
		struct DescriptorObservation *observation = &report.descriptor[descriptor];
		intptr_t handle = getOsHandle(descriptor);
		observation->before = (uint64_t)handle;
		observation->beforeType = GetFileType((HANDLE)handle);
		observation->openResult = -1;
		if (handle != (intptr_t)INVALID_HANDLE_VALUE && observation->beforeType != FILE_TYPE_UNKNOWN) {
			report.result = 24;
			goto cleanup;
		}
		*getCrtError() = 0;
		observation->closeResult = closeDescriptor(descriptor);
		observation->closeError = *getCrtError();
		*getCrtError() = 0;
		observation->openResult = openDescriptor("nul", _O_RDWR);
		observation->openError = *getCrtError();
		owned[descriptor] = observation->openResult;
		if (observation->openResult != descriptor) {
			report.result = 25;
			goto cleanup;
		}
		handle = getOsHandle(descriptor);
		observation->after = (uint64_t)handle;
		observation->afterType = GetFileType((HANDLE)handle);
		if (handle == (intptr_t)INVALID_HANDLE_VALUE || observation->afterType != FILE_TYPE_CHAR) {
			report.result = 26;
			goto cleanup;
		}
		++report.completed;
	}
	report.result = invalidParameters ? 27 : 0;
cleanup:
	if (closeDescriptor)
		for (unsigned index = 0; index < 3; ++index)
			if (owned[index] >= 0 && closeDescriptor(owned[index]) != 0)
				report.result = 28;
	report.invalidParameters = invalidParameters;
	if (installedHandler)
		setHandler(originalHandler);
	DWORD written = 0;
	if (!WriteFile(output, &report, sizeof(report), &written, NULL) || written != sizeof(report))
		report.result = 29;
	if (!CloseHandle(output))
		report.result = 30;
	// The process owns the loaded CRT until normal exit.
	return (int)report.result;
}

int wmain(int argc, WCHAR **argv) {
	if (argc == 3 && wcscmp(argv[1], L"--child") == 0)
		return childMain(argv[2]);
	if (argc != 1)
		return 10;
	int result = 1;
	WCHAR temporary[MAX_PATH], reportPath[MAX_PATH] = {0}, executable[32768], command[32768];
	PROCESS_INFORMATION process = {0};
	STARTUPINFOW startup = {0};
	HANDLE original[3];
	unsigned changed = 0;
	HANDLE reader = INVALID_HANDLE_VALUE;
	for (unsigned index = 0; index < 3; ++index)
		original[index] = GetStdHandle(selectors[index]);
	DWORD length = GetModuleFileNameW(NULL, executable, sizeof(executable) / sizeof(*executable));
	DWORD temporaryLength = GetTempPathW(MAX_PATH, temporary);
	if (!length || length >= sizeof(executable) / sizeof(*executable) || !temporaryLength ||
		temporaryLength >= MAX_PATH || !GetTempFileNameW(temporary, L"crt", 0, reportPath))
		goto cleanup;
	if (swprintf(command, sizeof(command) / sizeof(*command), L"\"%ls\" --child \"%ls\"", executable, reportPath) <= 0)
		goto cleanup;
	for (; changed < 3; ++changed)
		if (!SetStdHandle(selectors[changed], NULL))
			goto cleanup;
	startup.cb = sizeof(startup);
	if (!CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
		goto cleanup;
	while (changed) {
		--changed;
		if (!SetStdHandle(selectors[changed], original[changed]))
			goto cleanup;
	}
	DWORD exitCode = STILL_ACTIVE;
	DWORD wait = WaitForSingleObject(process.hProcess, 5000);
	if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &exitCode)) {
		fprintf(stderr, "CRT repair wait=%lu code=%lu pid=%lu\n", (unsigned long)wait, (unsigned long)exitCode,
				(unsigned long)process.dwProcessId);
		goto cleanup;
	}
	reader = CreateFileW(reportPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
						 FILE_ATTRIBUTE_NORMAL, NULL);
	struct RepairReport report = {0};
	DWORD received = 0;
	if (reader == INVALID_HANDLE_VALUE || !ReadFile(reader, &report, sizeof(report), &received, NULL) ||
		received != sizeof(report)) {
		fprintf(stderr, "CRT repair report absent; child=%lu\n", (unsigned long)exitCode);
		goto cleanup;
	}
	printf("CRT repair code=%lu result=%lu completed=%lu invalid=%lu\n", (unsigned long)exitCode,
		   (unsigned long)report.result, (unsigned long)report.completed, (unsigned long)report.invalidParameters);
	for (unsigned index = 0; index < 3; ++index) {
		const struct DescriptorObservation *observation = &report.descriptor[index];
		printf("fd%u NULL=%lu before=%llx/type%lu close=%d/error%d open=%d/error%d after=%llx/type%lu\n", index,
			   (unsigned long)report.standardNull[index], (unsigned long long)observation->before,
			   (unsigned long)observation->beforeType, observation->closeResult, observation->closeError,
			   observation->openResult, observation->openError, (unsigned long long)observation->after,
			   (unsigned long)observation->afterType);
	}
	if (exitCode || report.magic != 0x43525431 || report.result || report.completed != 3 || report.invalidParameters)
		goto cleanup;
	for (unsigned index = 0; index < 3; ++index)
		if (!report.standardNull[index] || report.descriptor[index].openResult != (int)index ||
			report.descriptor[index].afterType != FILE_TYPE_CHAR)
			goto cleanup;
	result = 0;
cleanup:
	while (changed) {
		--changed;
		if (!SetStdHandle(selectors[changed], original[changed]))
			result = 1;
	}
	if (process.hProcess && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process.hProcess, 31);
		DWORD reaped = WaitForSingleObject(process.hProcess, 5000);
		fprintf(stderr, "CRT repair cleanup terminate=%d wait=%lu pid=%lu\n", terminated, (unsigned long)reaped,
				(unsigned long)process.dwProcessId);
		result = 1;
	}
	if (reader != INVALID_HANDLE_VALUE && !CloseHandle(reader))
		result = 1;
	if (process.hThread && !CloseHandle(process.hThread))
		result = 1;
	if (process.hProcess && !CloseHandle(process.hProcess))
		result = 1;
	if (reportPath[0] && !DeleteFileW(reportPath))
		result = 1;
	if (result)
		fprintf(stderr, "CRT repair failed normally, error=%lu\n", (unsigned long)GetLastError());
	return result;
}
