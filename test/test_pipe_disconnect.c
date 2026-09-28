#include <windows.h>

#include <stdio.h>

#include "test_assert.h"

struct ClientArgs {
	const char *name;
	char sent;
	HANDLE client;
	DWORD error;
};

static DWORD WINAPI connect_client(LPVOID context) {
	struct ClientArgs *args = (struct ClientArgs *)context;
	Sleep(50);
	args->client = CreateFileA(args->name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (args->client == INVALID_HANDLE_VALUE) {
		args->error = GetLastError();
		return 1;
	}
	DWORD count = 0;
	if (!WriteFile(args->client, &args->sent, 1, &count, NULL) || count != 1) {
		args->error = GetLastError();
		return 2;
	}
	return 0;
}

int main(void) {
	char name[128];
	TEST_CHECK(snprintf(name, sizeof(name), "\\\\.\\pipe\\wibo-fixture-disconnect-%lu",
						(unsigned long)GetCurrentProcessId()) > 0);
	HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 1024,
									 1024, 0, NULL);
	TEST_CHECK(server != INVALID_HANDLE_VALUE);

	SetLastError(0x4321);
	TEST_CHECK(!DisconnectNamedPipe(INVALID_HANDLE_VALUE));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(!DisconnectNamedPipe(server));
	TEST_CHECK_EQ(ERROR_PIPE_LISTENING, GetLastError());

	for (int iteration = 0; iteration < 2; ++iteration) {
		struct ClientArgs args = {name, (char)('a' + iteration), INVALID_HANDLE_VALUE, ERROR_SUCCESS};
		HANDLE thread = CreateThread(NULL, 0, connect_client, &args, 0, NULL);
		TEST_CHECK(thread != NULL);
		TEST_CHECK(ConnectNamedPipe(server, NULL));
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
		DWORD threadResult = 99;
		TEST_CHECK(GetExitCodeThread(thread, &threadResult));
		TEST_CHECK_EQ(0, threadResult);
		TEST_CHECK(CloseHandle(thread));
		TEST_CHECK(args.client != INVALID_HANDLE_VALUE);
		char received = 0;
		DWORD count = 0;
		TEST_CHECK(ReadFile(server, &received, 1, &count, NULL));
		TEST_CHECK_EQ(1, count);
		TEST_CHECK_EQ(args.sent, received);
		TEST_CHECK(DisconnectNamedPipe(server));
		count = 99;
		BOOL readResult = ReadFile(args.client, &received, 1, &count, NULL);
		TEST_CHECK(!readResult || count == 0);
		TEST_CHECK(CloseHandle(args.client));
	}
	TEST_CHECK(CloseHandle(server));
	return 0;
}
