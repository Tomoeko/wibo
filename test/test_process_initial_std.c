#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

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

static int checkInherit(HANDLE handle) {
	DWORD flags = 0;
	SetLastError(0x4321);
	BOOL result = GetHandleInformation(handle, &flags);
	DWORD error = GetLastError();
	if (!result || !(flags & HANDLE_FLAG_INHERIT) || error != 0x4321) {
		fprintf(stderr, "standard inheritance: handle=%p result=%ld flags=0x%lx error=%lu\n", handle, (long)result,
				(unsigned long)flags, (unsigned long)error);
		return 0;
	}
	return 1;
}

static int childMain(char **arguments) {
	HANDLE input = parseHandle(arguments[2]), output = parseHandle(arguments[3]);
	DWORD expectedType = (DWORD)(uintptr_t)parseHandle(arguments[4]);
	if (GetStdHandle(STD_INPUT_HANDLE) != input || GetStdHandle(STD_OUTPUT_HANDLE) != output ||
		GetStdHandle(STD_ERROR_HANDLE) != output)
		return 21;
	if (!checkInherit(input) || !checkInherit(output) || GetFileType(input) != expectedType ||
		GetFileType(output) != FILE_TYPE_PIPE)
		return 22;
	DWORD transferred = 0;
	if (!WriteFile(output, "out", 3, &transferred, NULL) || transferred != 3 ||
		!WriteFile(GetStdHandle(STD_ERROR_HANDLE), "err", 3, &transferred, NULL) || transferred != 3)
		return 23;
	return 0;
}

static int selectInitialInput(void) {
	int result = 1, attributesInitialized = 0;
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE server = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE;
	STARTUPINFOEXA startup = {0};
	PROCESS_INFORMATION process = {0};
	char image[32768], command[32768 + 128], name[128];
	DWORD imageLength = GetModuleFileNameA(NULL, image, sizeof(image));
	DWORD inputType = GetFileType(input);
	if (!imageLength || imageLength >= sizeof(image) || inputType == FILE_TYPE_UNKNOWN)
		goto cleanup;
	int nameLength =
		snprintf(name, sizeof(name), "\\\\.\\pipe\\initial-standard-%lu", (unsigned long)GetCurrentProcessId());
	if (nameLength <= 0 || (size_t)nameLength >= sizeof(name))
		goto cleanup;
	server = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 4096,
							  5000, NULL);
	if (server == INVALID_HANDLE_VALUE)
		goto cleanup;
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	output = CreateFileA(name, GENERIC_WRITE, 0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (output == INVALID_HANDLE_VALUE || !checkInherit(output))
		goto cleanup;
	if (!ConnectNamedPipe(server, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
		goto cleanup;
	SIZE_T attributeBytes = 0;
	SetLastError(0);
	if (InitializeProcThreadAttributeList(NULL, 1, 0, &attributeBytes) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
		!attributeBytes)
		goto cleanup;
	startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attributeBytes);
	if (!startup.lpAttributeList || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes))
		goto cleanup;
	attributesInitialized = 1;
	HANDLE inherited[] = {input, output};
	if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
								   sizeof(inherited), NULL, NULL))
		goto cleanup;
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = input;
	startup.StartupInfo.hStdOutput = output;
	startup.StartupInfo.hStdError = output;
	int commandLength =
		snprintf(command, sizeof(command), "\"%s\" --child %llx %llx %lx", image, (unsigned long long)(uintptr_t)input,
				 (unsigned long long)(uintptr_t)output, (unsigned long)inputType);
	if (commandLength <= 0 || (size_t)commandLength >= sizeof(command) ||
		!CreateProcessA(image, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
						&startup.StartupInfo, &process))
		goto cleanup;
	if (!closeOwned(&output))
		goto cleanup;
	DWORD waited = WaitForSingleObject(process.hProcess, 5000), childResult = 0;
	if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &childResult) || childResult != 0) {
		fprintf(stderr, "selected standard child: wait=%lu result=%lu pid=%lu\n", (unsigned long)waited,
				(unsigned long)childResult, (unsigned long)process.dwProcessId);
		goto cleanup;
	}
	DWORD available = 0, transferred = 0;
	char bytes[6];
	if (!PeekNamedPipe(server, NULL, 0, NULL, &available, NULL) || available != sizeof(bytes) ||
		!ReadFile(server, bytes, sizeof(bytes), &transferred, NULL) || transferred != sizeof(bytes) ||
		memcmp(bytes, "outerr", sizeof(bytes)) || GetStdHandle(STD_INPUT_HANDLE) != input || !checkInherit(input))
		goto cleanup;
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "initial standard selection failed: error=%lu\n", (unsigned long)GetLastError());
	if (process.hProcess && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process.hProcess, 32);
		DWORD reaped = WaitForSingleObject(process.hProcess, 5000);
		fprintf(stderr, "child cleanup: terminate=%ld wait=%lu pid=%lu\n", (long)terminated, (unsigned long)reaped,
				(unsigned long)process.dwProcessId);
		result = 1;
	}
	if (startup.lpAttributeList) {
		if (attributesInitialized)
			DeleteProcThreadAttributeList(startup.lpAttributeList);
		free(startup.lpAttributeList);
	}
	int threadClosed = closeOwned(&process.hThread), processClosed = closeOwned(&process.hProcess);
	int outputClosed = closeOwned(&output), serverClosed = closeOwned(&server);
	return !result && threadClosed && processClosed && outputClosed && serverClosed;
}

int main(int argc, char **argv) {
	if (argc == 5 && !strcmp(argv[1], "--child"))
		return childMain(argv);
	TEST_CHECK(checkInherit(GetStdHandle(STD_INPUT_HANDLE)));
	TEST_CHECK(checkInherit(GetStdHandle(STD_OUTPUT_HANDLE)));
	TEST_CHECK(checkInherit(GetStdHandle(STD_ERROR_HANDLE)));
	TEST_CHECK(selectInitialInput());
	return 0;
}
