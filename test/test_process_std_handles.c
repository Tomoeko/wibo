#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const char inputBytes[] = "standard-input\n";
static const char outputBytes[] = "standard-output\n";
static const char errorBytes[] = "standard-error\n";

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
		else if (*text >= 'A' && *text <= 'F')
			digit = (unsigned)(*text - 'A' + 10);
		else
			return INVALID_HANDLE_VALUE;
		if (value > (UINTPTR_MAX - digit) / 16)
			return INVALID_HANDLE_VALUE;
		value = value * 16 + digit;
	}
	return (HANDLE)value;
}

static int childMain(char **argv) {
	HANDLE expectedInput = parseHandle(argv[2]);
	HANDLE expectedOutput = parseHandle(argv[3]);
	HANDLE excluded = parseHandle(argv[4]);
	if (GetStdHandle(STD_INPUT_HANDLE) != expectedInput || GetStdHandle(STD_OUTPUT_HANDLE) != expectedOutput ||
		GetStdHandle(STD_ERROR_HANDLE) != expectedOutput)
		return 21;
	// Process initialization may reuse an excluded slot for a non-file object.
	// The excluded file must not appear in the child file-handle table.
	SetLastError(0x4321);
	if (GetFileType(excluded) != FILE_TYPE_UNKNOWN || GetLastError() != ERROR_INVALID_HANDLE)
		return 22;
	char bytes[sizeof(inputBytes)] = {0};
	DWORD transferred = 0;
	if (!ReadFile(expectedInput, bytes, sizeof(inputBytes) - 1, &transferred, NULL) ||
		transferred != sizeof(inputBytes) - 1 || memcmp(bytes, inputBytes, sizeof(inputBytes) - 1))
		return 23;
	if (!WriteFile(expectedOutput, outputBytes, sizeof(outputBytes) - 1, &transferred, NULL) ||
		transferred != sizeof(outputBytes) - 1)
		return 24;
	if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), errorBytes, sizeof(errorBytes) - 1, &transferred, NULL) ||
		transferred != sizeof(errorBytes) - 1)
		return 25;
	return 0;
}

static int closeOwned(HANDLE *handle) {
	if (*handle == NULL || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE value = *handle;
	*handle = INVALID_HANDLE_VALUE;
	return CloseHandle(value) != FALSE;
}

static int testStandardFiles(void) {
	int result = 1;
	HANDLE originalInput = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE originalOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE originalError = GetStdHandle(STD_ERROR_HANDLE);
	WCHAR temporary[MAX_PATH], inputPath[MAX_PATH] = {0}, outputPath[MAX_PATH] = {0}, excludedPath[MAX_PATH] = {0};
	WCHAR executable[32768], command[32768];
	HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE, excluded = INVALID_HANDLE_VALUE;
	HANDLE reader = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION process = {0};
	STARTUPINFOEXW startup = {0};
	SIZE_T attributeBytes = 0;
	int attributesInitialized = 0;
	DWORD transferred = 0;
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	DWORD executableLength = GetModuleFileNameW(NULL, executable, sizeof(executable) / sizeof(*executable));
	if (!executableLength || executableLength >= sizeof(executable) / sizeof(*executable))
		goto cleanup;
	DWORD temporaryLength = GetTempPathW(MAX_PATH, temporary);
	if (!temporaryLength || temporaryLength >= MAX_PATH || !GetTempFileNameW(temporary, L"sti", 0, inputPath) ||
		!GetTempFileNameW(temporary, L"sto", 0, outputPath) || !GetTempFileNameW(temporary, L"ste", 0, excludedPath))
		goto cleanup;
	input = CreateFileW(inputPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	output = CreateFileW(outputPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
						 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	excluded = CreateFileW(excludedPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
						   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE || excluded == INVALID_HANDLE_VALUE ||
		!WriteFile(input, inputBytes, sizeof(inputBytes) - 1, &transferred, NULL) ||
		transferred != sizeof(inputBytes) - 1 || SetFilePointer(input, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
		goto cleanup;
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
	WCHAR suffix[128];
	int suffixLength = swprintf(suffix, sizeof(suffix) / sizeof(*suffix), L" --child %llx %llx %llx",
								(unsigned long long)(uintptr_t)input, (unsigned long long)(uintptr_t)output,
								(unsigned long long)(uintptr_t)excluded);
	if (suffixLength <= 0 || executableLength + (DWORD)suffixLength + 3 > sizeof(command) / sizeof(*command))
		goto cleanup;
	command[0] = L'"';
	wcscpy(command + 1, executable);
	command[executableLength + 1] = L'"';
	wcscpy(command + executableLength + 2, suffix);
	if (!CreateProcessW(executable, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
						&startup.StartupInfo, &process))
		goto cleanup;
	if (!closeOwned(&output))
		goto cleanup;
	DWORD wait = WaitForSingleObject(process.hProcess, 5000);
	if (wait != WAIT_OBJECT_0) {
		fprintf(stderr, "Child wait=%lu pid=%lu\n", (unsigned long)wait, (unsigned long)process.dwProcessId);
		goto cleanup;
	}
	DWORD childResult = 0;
	if (!GetExitCodeProcess(process.hProcess, &childResult) || childResult != 0) {
		fprintf(stderr, "Child result=%lu\n", (unsigned long)childResult);
		goto cleanup;
	}
	reader = CreateFileW(outputPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
						 FILE_ATTRIBUTE_NORMAL, NULL);
	char expected[sizeof(outputBytes) + sizeof(errorBytes) - 2];
	char actual[sizeof(expected) + 1];
	memcpy(expected, outputBytes, sizeof(outputBytes) - 1);
	memcpy(expected + sizeof(outputBytes) - 1, errorBytes, sizeof(errorBytes) - 1);
	if (reader == INVALID_HANDLE_VALUE || !ReadFile(reader, actual, sizeof(actual), &transferred, NULL) ||
		transferred != sizeof(expected) || memcmp(actual, expected, sizeof(expected)) ||
		GetStdHandle(STD_INPUT_HANDLE) != originalInput || GetStdHandle(STD_OUTPUT_HANDLE) != originalOutput ||
		GetStdHandle(STD_ERROR_HANDLE) != originalError)
		goto cleanup;
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "Standard stream inheritance failed, error=%lu\n", (unsigned long)GetLastError());
	if (process.hProcess && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process.hProcess, 32);
		DWORD reaped = WaitForSingleObject(process.hProcess, 5000);
		fprintf(stderr, "Cleanup terminate=%d reap=%lu pid=%lu\n", terminated, (unsigned long)reaped,
				(unsigned long)process.dwProcessId);
		result = 1;
	}
	if (startup.lpAttributeList) {
		if (attributesInitialized)
			DeleteProcThreadAttributeList(startup.lpAttributeList);
		free(startup.lpAttributeList);
	}
	int closedThread = closeOwned(&process.hThread);
	int closedProcess = closeOwned(&process.hProcess);
	if (!closedThread || !closedProcess)
		result = 1;
	int closedInput = closeOwned(&input);
	int closedOutput = closeOwned(&output);
	int closedExcluded = closeOwned(&excluded);
	int closedReader = closeOwned(&reader);
	if (!closedInput || !closedOutput || !closedExcluded || !closedReader)
		result = 1;
	int deletedInput = !inputPath[0] || DeleteFileW(inputPath);
	int deletedOutput = !outputPath[0] || DeleteFileW(outputPath);
	int deletedExcluded = !excludedPath[0] || DeleteFileW(excludedPath);
	if (!deletedInput || !deletedOutput || !deletedExcluded)
		result = 1;
	return result;
}

static int launchSelf(const WCHAR *mode, HANDLE first, HANDLE second, HANDLE third, HANDLE *inherited,
					  SIZE_T inheritedBytes, BOOL standardStreams, PROCESS_INFORMATION *process) {
	WCHAR executable[32768], command[32768], suffix[160];
	STARTUPINFOEXW startup = {0};
	SIZE_T attributeBytes = 0;
	int initialized = 0;
	int result = 0;
	DWORD length = GetModuleFileNameW(NULL, executable, sizeof(executable) / sizeof(*executable));
	if (!length || length >= sizeof(executable) / sizeof(*executable))
		return 0;
	int suffixLength = swprintf(suffix, sizeof(suffix) / sizeof(*suffix), L" %ls %llx %llx %llx", mode,
								(unsigned long long)(uintptr_t)first, (unsigned long long)(uintptr_t)second,
								(unsigned long long)(uintptr_t)third);
	if (suffixLength <= 0 || length + (DWORD)suffixLength + 3 > sizeof(command) / sizeof(*command))
		return 0;
	command[0] = L'"';
	wcscpy(command + 1, executable);
	command[length + 1] = L'"';
	wcscpy(command + length + 2, suffix);
	if (InitializeProcThreadAttributeList(NULL, 1, 0, &attributeBytes) || !attributeBytes ||
		GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return 0;
	startup.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attributeBytes);
	if (!startup.lpAttributeList || !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes))
		goto cleanup;
	initialized = 1;
	if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
								   inheritedBytes, NULL, NULL))
		goto cleanup;
	startup.StartupInfo.cb = sizeof(startup);
	if (standardStreams) {
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = first;
		startup.StartupInfo.hStdOutput = second;
		startup.StartupInfo.hStdError = second;
	}
	result = CreateProcessW(executable, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
							&startup.StartupInfo, process) != FALSE;
cleanup:
	if (initialized)
		DeleteProcThreadAttributeList(startup.lpAttributeList);
	free(startup.lpAttributeList);
	return result;
}

static int finishChild(PROCESS_INFORMATION *process, DWORD timeout) {
	DWORD wait = WaitForSingleObject(process->hProcess, timeout);
	DWORD code = STILL_ACTIVE;
	int result = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process->hProcess, &code) && code == 0;
	if (!result)
		fprintf(stderr, "Child completion wait=%lu code=%lu pid=%lu\n", (unsigned long)wait, (unsigned long)code,
				(unsigned long)process->dwProcessId);
	return result;
}

static int disposeChild(PROCESS_INFORMATION *process) {
	int result = 1;
	if (process->hProcess && WaitForSingleObject(process->hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process->hProcess, 32);
		DWORD reaped = WaitForSingleObject(process->hProcess, 5000);
		fprintf(stderr, "Child cleanup terminate=%d reap=%lu pid=%lu\n", terminated, (unsigned long)reaped,
				(unsigned long)process->dwProcessId);
		result = 0;
	}
	int closedThread = closeOwned(&process->hThread);
	int closedProcess = closeOwned(&process->hProcess);
	return result && closedThread && closedProcess;
}

static int positionIs(HANDLE handle, LONGLONG expected) {
	LARGE_INTEGER distance = {{0}}, position;
	return SetFilePointerEx(handle, distance, &position, FILE_CURRENT) && position.QuadPart == expected;
}

static int cursorChild(char **argv) {
	HANDLE file = parseHandle(argv[2]);
	HANDLE alias = parseHandle(argv[3]);
	char bytes[2];
	DWORD transferred;
	if (file == alias || !positionIs(file, 3) || !positionIs(alias, 3))
		return 41;
	if (!ReadFile(file, bytes, 2, &transferred, NULL) || transferred != 2 || memcmp(bytes, "34", 2) ||
		!positionIs(alias, 5))
		return 42;
	if (!ReadFile(alias, bytes, 2, &transferred, NULL) || transferred != 2 || memcmp(bytes, "56", 2) ||
		!positionIs(file, 7))
		return 43;
	LARGE_INTEGER distance;
	distance.QuadPart = 8;
	if (!SetFilePointerEx(alias, distance, NULL, FILE_BEGIN) || !ReadFile(file, bytes, 1, &transferred, NULL) ||
		transferred != 1 || bytes[0] != '8' || !positionIs(alias, 9))
		return 44;
	return 0;
}

static int testSharedCursor(void) {
	WCHAR temporary[MAX_PATH], path[MAX_PATH] = {0};
	HANDLE file = INVALID_HANDLE_VALUE, alias = INVALID_HANDLE_VALUE;
	PROCESS_INFORMATION process = {0};
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	int result = 1;
	DWORD length = GetTempPathW(MAX_PATH, temporary);
	if (!length || length >= MAX_PATH || !GetTempFileNameW(temporary, L"cur", 0, path))
		goto cleanup;
	file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
					   FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD transferred;
	if (file == INVALID_HANDLE_VALUE || !WriteFile(file, "0123456789", 10, &transferred, NULL) || transferred != 10 ||
		!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &alias, 0, TRUE, DUPLICATE_SAME_ACCESS))
		goto cleanup;
	LARGE_INTEGER distance;
	distance.QuadPart = 3;
	if (!SetFilePointerEx(file, distance, NULL, FILE_BEGIN))
		goto cleanup;
	HANDLE inherited[] = {file, alias};
	if (!launchSelf(L"--cursor-child", file, alias, NULL, inherited, sizeof(inherited), FALSE, &process) ||
		!finishChild(&process, 5000))
		goto cleanup;
	char byte;
	if (!positionIs(file, 9) || !positionIs(alias, 9) || !ReadFile(alias, &byte, 1, &transferred, NULL) ||
		transferred != 1 || byte != '9' || !positionIs(file, 10))
		goto cleanup;
	result = 0;
cleanup:
	if (process.hProcess && !disposeChild(&process))
		result = 1;
	int fileClosed = closeOwned(&file);
	int aliasClosed = closeOwned(&alias);
	int deleted = !path[0] || DeleteFileW(path);
	if (!fileClosed || !aliasClosed || !deleted)
		result = 1;
	fprintf(stderr, "Shared cursor result=%d\n", result);
	return result;
}

struct PipeCapture {
	HANDLE read;
	char bytes[64];
	DWORD length;
	DWORD error;
	int overflow;
};

// Static storage stays valid even if a failed bounded join leaves a blocked reader.
static struct PipeCapture pipeCapture;

static DWORD WINAPI capturePipe(LPVOID argument) {
	struct PipeCapture *capture = argument;
	for (;;) {
		DWORD transferred = 0;
		DWORD available = sizeof(capture->bytes) - capture->length;
		if (!available) {
			capture->overflow = 1;
			return 1;
		}
		if (!ReadFile(capture->read, capture->bytes + capture->length, available, &transferred, NULL)) {
			capture->error = GetLastError();
			return capture->error == ERROR_BROKEN_PIPE ? 0 : 2;
		}
		if (!transferred)
			return 3;
		capture->length += transferred;
	}
}

static int pipeChild(char **argv) {
	HANDLE input = parseHandle(argv[2]);
	HANDLE output = parseHandle(argv[3]);
	HANDLE excludedWriter = parseHandle(argv[4]);
	if (GetStdHandle(STD_INPUT_HANDLE) != input || GetStdHandle(STD_OUTPUT_HANDLE) != output ||
		GetStdHandle(STD_ERROR_HANDLE) != output)
		return 51;
	SetLastError(0);
	if (GetFileType(excludedWriter) != FILE_TYPE_UNKNOWN || GetLastError() != ERROR_INVALID_HANDLE)
		return 52;
	DWORD transferred;
	if (!WriteFile(output, outputBytes, sizeof(outputBytes) - 1, &transferred, NULL) ||
		transferred != sizeof(outputBytes) - 1 ||
		!WriteFile(GetStdHandle(STD_ERROR_HANDLE), errorBytes, sizeof(errorBytes) - 1, &transferred, NULL) ||
		transferred != sizeof(errorBytes) - 1 || !CloseHandle(output))
		return 53;
	// The parent must see EOF while this process is still alive and waiting here.
	char release;
	if (!ReadFile(input, &release, 1, &transferred, NULL) || transferred != 1 || release != 'R')
		return 54;
	return 0;
}

static int testPipeEof(void) {
	HANDLE inputRead = INVALID_HANDLE_VALUE, inputWrite = INVALID_HANDLE_VALUE;
	HANDLE outputRead = INVALID_HANDLE_VALUE, outputWrite = INVALID_HANDLE_VALUE;
	HANDLE excludedWriter = INVALID_HANDLE_VALUE, readerThread = NULL;
	PROCESS_INFORMATION process = {0};
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	int result = 1;
	int readerJoined = 0;
	BOOL created = CreatePipe(&inputRead, &inputWrite, &security, 4096);
	if (!created) {
		inputRead = inputWrite = INVALID_HANDLE_VALUE;
		goto cleanup;
	}
	created = CreatePipe(&outputRead, &outputWrite, &security, 4096);
	if (!created) {
		outputRead = outputWrite = INVALID_HANDLE_VALUE;
		goto cleanup;
	}
	if (!SetHandleInformation(inputWrite, HANDLE_FLAG_INHERIT, 0) ||
		!SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0) ||
		!DuplicateHandle(GetCurrentProcess(), outputWrite, GetCurrentProcess(), &excludedWriter, 0, TRUE,
						 DUPLICATE_SAME_ACCESS))
		goto cleanup;
	HANDLE inherited[] = {inputRead, outputWrite};
	if (!launchSelf(L"--pipe-child", inputRead, outputWrite, excludedWriter, inherited, sizeof(inherited), TRUE,
					&process))
		goto cleanup;
	int inputClosed = closeOwned(&inputRead);
	int outputClosed = closeOwned(&outputWrite);
	int excludedClosed = closeOwned(&excludedWriter);
	if (!inputClosed || !outputClosed || !excludedClosed)
		goto cleanup;
	memset(&pipeCapture, 0, sizeof(pipeCapture));
	pipeCapture.read = outputRead;
	readerThread = CreateThread(NULL, 0, capturePipe, &pipeCapture, 0, NULL);
	if (!readerThread)
		goto cleanup;
	DWORD waited = WaitForSingleObject(readerThread, 3000);
	readerJoined = waited == WAIT_OBJECT_0;
	if (!readerJoined || WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT)
		goto cleanup;
	char expected[sizeof(outputBytes) + sizeof(errorBytes) - 2];
	memcpy(expected, outputBytes, sizeof(outputBytes) - 1);
	memcpy(expected + sizeof(outputBytes) - 1, errorBytes, sizeof(errorBytes) - 1);
	if (pipeCapture.overflow || pipeCapture.error != ERROR_BROKEN_PIPE || pipeCapture.length != sizeof(expected) ||
		memcmp(pipeCapture.bytes, expected, sizeof(expected)))
		goto cleanup;
	result = 0;
cleanup:
	int releaseSent = 1;
	if (process.hProcess && inputWrite != INVALID_HANDLE_VALUE) {
		DWORD transferred;
		releaseSent = WriteFile(inputWrite, "R", 1, &transferred, NULL) && transferred == 1;
	}
	int controlClosed = closeOwned(&inputWrite);
	if (process.hProcess) {
		int childFinished = finishChild(&process, 5000);
		int childDisposed = disposeChild(&process);
		if (!releaseSent || !childFinished || !childDisposed)
			result = 1;
	}
	// Close any writer still owned after an early failure before joining the reader.
	int outputClosedFinal = closeOwned(&outputWrite);
	int excludedClosedFinal = closeOwned(&excludedWriter);
	if (readerThread && !readerJoined)
		readerJoined = WaitForSingleObject(readerThread, 5000) == WAIT_OBJECT_0;
	if (readerThread && !readerJoined) {
		fprintf(stderr, "Pipe reader exceeded cleanup deadline\n");
		ExitProcess(90);
	}
	int readerClosed = closeOwned(&readerThread);
	int inputClosedFinal = closeOwned(&inputRead);
	int readClosed = closeOwned(&outputRead);
	if (!controlClosed || !outputClosedFinal || !excludedClosedFinal || !readerClosed || !inputClosedFinal ||
		!readClosed)
		result = 1;
	fprintf(stderr, "Pipe EOF result=%d\n", result);
	return result;
}

int main(int argc, char **argv) {
	if (argc == 5 && strcmp(argv[1], "--child") == 0)
		return childMain(argv);
	if (argc == 5 && strcmp(argv[1], "--cursor-child") == 0)
		return cursorChild(argv);
	if (argc == 5 && strcmp(argv[1], "--pipe-child") == 0)
		return pipeChild(argv);
	if (argc != 1)
		return 2;
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
	int basicResult = testStandardFiles();
	int cursorResult = testSharedCursor();
	int pipeResult = testPipeEof();
	if (GetStdHandle(STD_INPUT_HANDLE) != input || GetStdHandle(STD_OUTPUT_HANDLE) != output ||
		GetStdHandle(STD_ERROR_HANDLE) != error)
		return 3;
	return basicResult || cursorResult || pipeResult;
}
