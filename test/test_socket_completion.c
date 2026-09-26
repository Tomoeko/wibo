#include <winsock2.h>

#include "test_assert.h"
#include <windows.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	const ULONG_PTR key = ((ULONG_PTR)1 << (sizeof(ULONG_PTR) * 8 - 1)) | 0x71;
	HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	HANDLE other = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	TEST_CHECK(port && other);
	for (unsigned flags = 0; flags < 2; ++flags) {
		SOCKET s = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, flags);
		TEST_CHECK(s != INVALID_SOCKET);
		SetLastError(0x731);
		HANDLE associated = CreateIoCompletionPort((HANDLE)s, port, key, 0);
		if (flags) {
			TEST_CHECK(associated == port);
			TEST_CHECK_EQ(0x731, GetLastError());
			TEST_CHECK(CreateIoCompletionPort((HANDLE)s, port, key + 1, 0) == NULL);
			TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
			TEST_CHECK(CreateIoCompletionPort((HANDLE)s, other, key, 0) == NULL);
			TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		} else {
			TEST_CHECK(associated == NULL);
			TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		}
		struct sockaddr_in address = {0};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		TEST_CHECK_EQ(0, bind(s, (const struct sockaddr *)&address, sizeof(address)));
		TEST_CHECK_EQ(0, listen(s, 1));
		int length = sizeof(address);
		TEST_CHECK_EQ(0, getsockname(s, (struct sockaddr *)&address, &length));
		SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		TEST_CHECK(client != INVALID_SOCKET);
		TEST_CHECK_EQ(0, connect(client, (const struct sockaddr *)&address, sizeof(address)));
		SOCKET accepted = accept(s, NULL, NULL);
		TEST_CHECK(accepted != INVALID_SOCKET);
		associated = CreateIoCompletionPort((HANDLE)accepted, other, key + 2, 0);
		if (flags)
			TEST_CHECK(associated == other);
		else {
			TEST_CHECK(associated == NULL);
			TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		}
		TEST_CHECK_EQ(0, closesocket(accepted));
		TEST_CHECK_EQ(0, closesocket(client));
		TEST_CHECK_EQ(0, closesocket(s));
		TEST_CHECK(CreateIoCompletionPort((HANDLE)s, port, key, 0) == NULL);
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	}
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	TEST_CHECK(s != INVALID_SOCKET);
	HANDLE created = CreateIoCompletionPort((HANDLE)s, NULL, key, 1);
	TEST_CHECK(created != NULL);
	TEST_CHECK_EQ(0, closesocket(s));
	TEST_CHECK(CloseHandle(created));
	DWORD bytes = 0x51;
	ULONG_PTR receivedKey = 0x61;
	OVERLAPPED *operation = (OVERLAPPED *)(ULONG_PTR)0x71;
	TEST_CHECK(!GetQueuedCompletionStatus(port, &bytes, &receivedKey, &operation, 0));
	TEST_CHECK_EQ(WAIT_TIMEOUT, GetLastError());
	TEST_CHECK(operation == NULL);
	TEST_CHECK_EQ(0x51, bytes);
	TEST_CHECK_U64_EQ(0x61, receivedKey);
	TEST_CHECK(CloseHandle(other));
	TEST_CHECK(CloseHandle(port));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
