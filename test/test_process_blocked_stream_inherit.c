#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

enum { kWriteBytes = 1024 * 1024 };

struct StreamOperation {
	HANDLE handle;
	HANDLE ready;
	BOOL write;
	BYTE *bytes;
	DWORD count;
	DWORD transferred;
	DWORD error;
	BOOL success;
};

struct ChildOperation {
	char image[32768];
	char command[32768 + 160];
	STARTUPINFOEXA startup;
	PROCESS_INFORMATION process;
	BOOL success;
	DWORD error;
};

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE value = *handle;
	*handle = INVALID_HANDLE_VALUE;
	return CloseHandle(value) != FALSE;
}

static HANDLE parseHandle(const char *text) {
	uintptr_t value = 0;
	if (!*text)
		return INVALID_HANDLE_VALUE;
	for (; *text; ++text) {
		unsigned digit;
		if (*text >= '0' && *text <= '9')
			digit = (unsigned)(*text - '0');
		else if (*text >= 'a' && *text <= 'f')
			digit = (unsigned)(*text - 'a' + 10);
		else
			return INVALID_HANDLE_VALUE;
		if (value > (UINTPTR_MAX - digit) / 16)
			return INVALID_HANDLE_VALUE;
		value = value * 16 + digit;
	}
	return (HANDLE)value;
}

static DWORD WINAPI operateStream(void *argument) {
	struct StreamOperation *operation = argument;
	if (!SetEvent(operation->ready)) {
		operation->error = GetLastError();
		return 1;
	}
	SetLastError(0x4321);
	if (operation->write)
		operation->success =
			WriteFile(operation->handle, operation->bytes, operation->count, &operation->transferred, NULL);
	else
		operation->success =
			ReadFile(operation->handle, operation->bytes, operation->count, &operation->transferred, NULL);
	operation->error = GetLastError();
	return 0;
}

static DWORD WINAPI drainStream(void *argument) {
	struct StreamOperation *operation = argument;
	BYTE bytes[4096];
	operation->success = TRUE;
	while (operation->transferred < operation->count) {
		DWORD transferred = 0;
		DWORD remaining = operation->count - operation->transferred;
		DWORD requested = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
		if (!ReadFile(operation->handle, bytes, requested, &transferred, NULL) || !transferred) {
			operation->success = FALSE;
			operation->error = GetLastError();
			break;
		}
		for (DWORD index = 0; index < transferred; ++index) {
			if (bytes[index] != 0x5a)
				operation->success = FALSE;
		}
		operation->transferred += transferred;
	}
	return operation->success ? 0 : 1;
}

static DWORD WINAPI startChild(void *argument) {
	struct ChildOperation *operation = argument;
	SetLastError(0x4321);
	operation->success =
		CreateProcessA(operation->image, operation->command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
					   &operation->startup.StartupInfo, &operation->process);
	operation->error = GetLastError();
	return operation->success ? 0 : 1;
}

static int childMain(char **arguments) {
	BOOL write = strcmp(arguments[2], "write") == 0;
	HANDLE stream = parseHandle(arguments[3]);
	HANDLE report = parseHandle(arguments[4]);
	if (stream == INVALID_HANDLE_VALUE || report == INVALID_HANDLE_VALUE ||
		GetStdHandle(STD_INPUT_HANDLE) != (write ? report : stream) ||
		GetStdHandle(STD_OUTPUT_HANDLE) != (write ? stream : report) || GetStdHandle(STD_ERROR_HANDLE) != report ||
		GetFileType(stream) != FILE_TYPE_PIPE || GetFileType(report) != FILE_TYPE_DISK)
		return 21;
	DWORD transferred = 0;
	return WriteFile(report, "child-started", 13, &transferred, NULL) && transferred == 13 ? 0 : 22;
}

static int checkBlockedStream(BOOL write) {
	int result = 1, attributesInitialized = 0;
	char temporary[MAX_PATH], reportPath[MAX_PATH] = {0};
	HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE, report = INVALID_HANDLE_VALUE;
	HANDLE operationThread = NULL, childThread = NULL, drainThread = NULL;
	struct StreamOperation operation = {0}, drain = {0};
	struct ChildOperation child = {0};
	BYTE readByte = 0;
	SIZE_T attributeBytes = 0;
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	DWORD length = GetModuleFileNameA(NULL, child.image, sizeof(child.image));
	DWORD temporaryLength = GetTempPathA(sizeof(temporary), temporary);
	if (!length || length >= sizeof(child.image) || !temporaryLength || temporaryLength >= sizeof(temporary) ||
		!GetTempFileNameA(temporary, "bsi", 0, reportPath) || !CreatePipe(&input, &output, &security, 4096))
		goto cleanup;
	report = CreateFileA(reportPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
						 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (report == INVALID_HANDLE_VALUE || !SetHandleInformation(write ? input : output, HANDLE_FLAG_INHERIT, 0))
		goto cleanup;
	operation.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
	operation.handle = write ? output : input;
	operation.write = write;
	operation.count = write ? kWriteBytes : 1;
	operation.bytes = write ? malloc(kWriteBytes) : &readByte;
	if (!operation.ready || !operation.bytes)
		goto cleanup;
	if (write)
		memset(operation.bytes, 0x5a, kWriteBytes);
	SetLastError(0);
	if (InitializeProcThreadAttributeList(NULL, 1, 0, &attributeBytes) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
		!attributeBytes)
		goto cleanup;
	child.startup.lpAttributeList = malloc(attributeBytes);
	if (!child.startup.lpAttributeList ||
		!InitializeProcThreadAttributeList(child.startup.lpAttributeList, 1, 0, &attributeBytes))
		goto cleanup;
	attributesInitialized = 1;
	HANDLE inherited[] = {operation.handle, report};
	if (!UpdateProcThreadAttribute(child.startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
								   sizeof(inherited), NULL, NULL))
		goto cleanup;
	child.startup.StartupInfo.cb = sizeof(child.startup);
	child.startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	child.startup.StartupInfo.hStdInput = write ? report : input;
	child.startup.StartupInfo.hStdOutput = write ? output : report;
	child.startup.StartupInfo.hStdError = report;
	int commandLength = snprintf(child.command, sizeof(child.command), "\"%s\" --child %s %llx %llx", child.image,
								 write ? "write" : "read", (unsigned long long)(uintptr_t)operation.handle,
								 (unsigned long long)(uintptr_t)report);
	if (commandLength <= 0 || (size_t)commandLength >= sizeof(child.command))
		goto cleanup;
	operationThread = CreateThread(NULL, 0, operateStream, &operation, 0, NULL);
	if (!operationThread || WaitForSingleObject(operation.ready, 5000) != WAIT_OBJECT_0)
		goto cleanup;
	// No data is supplied or drained during this bounded scheduling interval.
	Sleep(100);
	if (WaitForSingleObject(operationThread, 0) != WAIT_TIMEOUT)
		goto cleanup;
	childThread = CreateThread(NULL, 0, startChild, &child, 0, NULL);
	if (!childThread || WaitForSingleObject(childThread, 2000) != WAIT_OBJECT_0 || !child.success ||
		WaitForSingleObject(child.process.hProcess, 2000) != WAIT_OBJECT_0)
		goto cleanup;
	DWORD childResult = 0;
	if (!GetExitCodeProcess(child.process.hProcess, &childResult) || childResult ||
		WaitForSingleObject(operationThread, 0) != WAIT_TIMEOUT)
		goto cleanup;
	result = 0;
cleanup:
	// Release the stream before joining a spawn worker that may be waiting on it.
	if (operationThread && write) {
		drain.handle = input;
		drain.count = kWriteBytes;
		drainThread = CreateThread(NULL, 0, drainStream, &drain, 0, NULL);
		if (!drainThread) {
			closeOwned(&input);
			result = 2;
		}
	} else if (operationThread) {
		DWORD transferred = 0;
		if (!WriteFile(output, "!", 1, &transferred, NULL) || transferred != 1)
			result = 2;
		closeOwned(&output);
	}
	if (operationThread && WaitForSingleObject(operationThread, 5000) != WAIT_OBJECT_0)
		ExitProcess(70);
	if (childThread && WaitForSingleObject(childThread, 5000) != WAIT_OBJECT_0)
		ExitProcess(71);
	if (child.process.hProcess && WaitForSingleObject(child.process.hProcess, 0) != WAIT_OBJECT_0) {
		if (!TerminateProcess(child.process.hProcess, 72) ||
			WaitForSingleObject(child.process.hProcess, 5000) != WAIT_OBJECT_0)
			ExitProcess(73);
		result = 3;
	}
	closeOwned(&output);
	if (drainThread && WaitForSingleObject(drainThread, 5000) != WAIT_OBJECT_0)
		ExitProcess(74);
	if (!result && (!operation.success || operation.transferred != operation.count || (!write && readByte != '!') ||
					(write && (!drain.success || drain.transferred != kWriteBytes))))
		result = 4;
	if (!result) {
		char bytes[13];
		DWORD transferred = 0;
		if (SetFilePointer(report, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
			!ReadFile(report, bytes, sizeof(bytes), &transferred, NULL) || transferred != sizeof(bytes) ||
			memcmp(bytes, "child-started", sizeof(bytes)))
			result = 5;
	}
	if (child.startup.lpAttributeList) {
		if (attributesInitialized)
			DeleteProcThreadAttributeList(child.startup.lpAttributeList);
		free(child.startup.lpAttributeList);
	}
	if (write)
		free(operation.bytes);
	int closed = closeOwned(&drainThread) & closeOwned(&operationThread) & closeOwned(&childThread) &
				 closeOwned(&operation.ready) & closeOwned(&child.process.hThread) &
				 closeOwned(&child.process.hProcess) & closeOwned(&input) & closeOwned(&output) & closeOwned(&report);
	if (!closed || (reportPath[0] && !DeleteFileA(reportPath)))
		result = 6;
	if (result)
		fprintf(stderr, "blocked stream inheritance: write=%ld result=%d spawn-error=%lu io-error=%lu\n", (long)write,
				result, (unsigned long)child.error, (unsigned long)operation.error);
	return result;
}

int main(int argc, char **argv) {
	if (argc == 5 && strcmp(argv[1], "--child") == 0)
		return childMain(argv);
	TEST_CHECK_EQ(1, argc);
	TEST_CHECK_EQ(0, checkBlockedStream(FALSE));
	TEST_CHECK_EQ(0, checkBlockedStream(TRUE));
	printf("blocked_stream_inheritance=2\n");
	return 0;
}
