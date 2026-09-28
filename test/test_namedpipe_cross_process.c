#include "test_assert.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void pipe_name(WCHAR *name, size_t capacity, unsigned long id, unsigned long nonce, const WCHAR *kind) {
	TEST_CHECK(swprintf(name, capacity, L"\\\\.\\pipe\\wibo_cross_%lu_%lu_%ls_\u00e9", id, nonce, kind) > 0);
}

static void finish_io(HANDLE pipe, OVERLAPPED *operation, BOOL immediate, DWORD *transferred) {
	if (!immediate) {
		TEST_CHECK_EQ(ERROR_IO_PENDING, GetLastError());
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(operation->hEvent, 5000));
		BOOL completed = GetOverlappedResult(pipe, operation, transferred, FALSE);
		TEST_CHECK_MSG(completed, "overlapped completion failed: %lu", (unsigned long)GetLastError());
	}
}

int main(int argc, char **argv) {
	if (argc == 4 && strcmp(argv[1], "child") == 0) {
		WCHAR name[256];
		pipe_name(name, 256, strtoul(argv[2], NULL, 10), strtoul(argv[3], NULL, 10), L"data");
		HANDLE client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		TEST_CHECK(client != INVALID_HANDLE_VALUE);
		DWORD count = 0;
		TEST_CHECK(WriteFile(client, "ping", 4, &count, NULL));
		TEST_CHECK_EQ(4, count);
		char reply[4] = {0};
		BOOL readSucceeded = ReadFile(client, reply, sizeof(reply), &count, NULL);
		TEST_CHECK_MSG(readSucceeded, "child reply read failed: %lu", (unsigned long)GetLastError());
		TEST_CHECK_EQ(4, count);
		TEST_CHECK(memcmp(reply, "pong", 4) == 0);
		TEST_CHECK(CloseHandle(client));
		return 0;
	}

	const unsigned long id = GetCurrentProcessId();
	const unsigned long nonce = GetTickCount();
	WCHAR name[256];
	pipe_name(name, 256, id, nonce, L"cancel");
	HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
								   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1024, 1024, 0, NULL);
	TEST_CHECK(pipe != INVALID_HANDLE_VALUE);
	OVERLAPPED cancelled = {0};
	cancelled.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(cancelled.hEvent != NULL);
	TEST_CHECK(!ConnectNamedPipe(pipe, &cancelled));
	TEST_CHECK_EQ(ERROR_IO_PENDING, GetLastError());
	TEST_CHECK(CancelIo(pipe));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(cancelled.hEvent, 5000));
	DWORD count = 0;
	TEST_CHECK(!GetOverlappedResult(pipe, &cancelled, &count, FALSE));
	TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, GetLastError());
	TEST_CHECK(CloseHandle(cancelled.hEvent));
	TEST_CHECK(CloseHandle(pipe));
	pipe_name(name, 256, id, nonce, L"cancelEx");
	pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
							PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1024, 1024, 0, NULL);
	TEST_CHECK(pipe != INVALID_HANDLE_VALUE);
	OVERLAPPED cancelledEx = {0};
	cancelledEx.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(cancelledEx.hEvent != NULL);
	TEST_CHECK(!ConnectNamedPipe(pipe, &cancelledEx));
	TEST_CHECK_EQ(ERROR_IO_PENDING, GetLastError());
	TEST_CHECK(CancelIoEx(pipe, &cancelledEx));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(cancelledEx.hEvent, 5000));
	TEST_CHECK(!GetOverlappedResult(pipe, &cancelledEx, &count, FALSE));
	TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, GetLastError());
	TEST_CHECK(CloseHandle(cancelledEx.hEvent));
	TEST_CHECK(CloseHandle(pipe));

	pipe_name(name, 256, id, nonce, L"data");
	pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
							PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1024, 1024, 0, NULL);
	TEST_CHECK(pipe != INVALID_HANDLE_VALUE);
	OVERLAPPED connected = {0};
	connected.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(connected.hEvent != NULL);
	TEST_CHECK(!ConnectNamedPipe(pipe, &connected));
	TEST_CHECK_EQ(ERROR_IO_PENDING, GetLastError());

	char image[MAX_PATH];
	DWORD imageLength = GetModuleFileNameA(NULL, image, sizeof(image));
	TEST_CHECK(imageLength > 0 && imageLength < sizeof(image));
	char command[MAX_PATH + 80];
	TEST_CHECK(snprintf(command, sizeof(command), "\"%s\" child %lu %lu", image, id, nonce) > 0);
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	TEST_CHECK(CreateProcessA(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(connected.hEvent, 5000));
	TEST_CHECK(GetOverlappedResult(pipe, &connected, &count, FALSE));
	TEST_CHECK_EQ(0, count);

	char request[4] = {0};
	OVERLAPPED readOperation = {0};
	readOperation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(readOperation.hEvent != NULL);
	BOOL immediate = ReadFile(pipe, request, sizeof(request), &count, &readOperation);
	finish_io(pipe, &readOperation, immediate, &count);
	TEST_CHECK_EQ(4, count);
	TEST_CHECK(memcmp(request, "ping", 4) == 0);

	OVERLAPPED writeOperation = {0};
	writeOperation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(writeOperation.hEvent != NULL);
	immediate = WriteFile(pipe, "pong", 4, &count, &writeOperation);
	finish_io(pipe, &writeOperation, immediate, &count);
	TEST_CHECK_EQ(4, count);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 5000));
	DWORD exitCode = 0;
	TEST_CHECK(GetExitCodeProcess(process.hProcess, &exitCode));
	TEST_CHECK_EQ(0, exitCode);
	TEST_CHECK(CloseHandle(process.hThread));
	TEST_CHECK(CloseHandle(process.hProcess));
	TEST_CHECK(CloseHandle(writeOperation.hEvent));
	TEST_CHECK(CloseHandle(readOperation.hEvent));
	TEST_CHECK(CloseHandle(connected.hEvent));
	TEST_CHECK(CloseHandle(pipe));
	return 0;
}
