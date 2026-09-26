#include <winsock2.h>

#include "test_assert.h"
#include <windows.h>

struct Reads {
	SOCKET socket;
	HANDLE ready, release;
	OVERLAPPED operations[4];
	char bytes[4];
};

static void queueRead(struct Reads *reads, unsigned index) {
	WSABUF buffer = {1, reads->bytes + index};
	DWORD flags = 0;
	TEST_CHECK_EQ(SOCKET_ERROR, WSARecv(reads->socket, &buffer, 1, NULL, &flags, reads->operations + index, NULL));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
}

static DWORD WINAPI readThread(LPVOID argument) {
	struct Reads *reads = (struct Reads *)argument;
	queueRead(reads, 0);
	queueRead(reads, 1);
	TEST_CHECK(SetEvent(reads->ready));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(reads->release, 5000));
	return 0;
}

static OVERLAPPED *completion(HANDLE port, OVERLAPPED *expected, BOOL success, DWORD bytes) {
	DWORD transferred = 0;
	ULONG_PTR key = 0;
	OVERLAPPED *operation = NULL;
	TEST_CHECK_EQ(success, GetQueuedCompletionStatus(port, &transferred, &key, &operation, 5000));
	if (!success)
		TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, GetLastError());
	TEST_CHECK_EQ(7, key);
	TEST_CHECK_EQ(bytes, transferred);
	if (expected)
		TEST_CHECK(operation == expected);
	return operation;
}

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	TEST_CHECK_EQ(0, bind(listener, (const struct sockaddr *)&address, sizeof(address)));
	TEST_CHECK_EQ(0, listen(listener, 1));
	int length = sizeof(address);
	TEST_CHECK_EQ(0, getsockname(listener, (struct sockaddr *)&address, &length));
	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK_EQ(0, connect(client, (const struct sockaddr *)&address, sizeof(address)));
	struct Reads reads = {0};
	reads.socket = accept(listener, NULL, NULL);
	TEST_CHECK(reads.socket != INVALID_SOCKET);
	TEST_CHECK_EQ(0, closesocket(listener));
	HANDLE port = CreateIoCompletionPort((HANDLE)reads.socket, NULL, 7, 1);
	TEST_CHECK(port != NULL);
	reads.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
	reads.release = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(reads.ready && reads.release);
	HANDLE thread = CreateThread(NULL, 0, readThread, &reads, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(reads.ready, 5000));
	TEST_CHECK(CancelIoEx((HANDLE)reads.socket, reads.operations));
	completion(port, reads.operations, FALSE, 0);
	TEST_CHECK_EQ(1, send(client, "!", 1, 0));
	completion(port, reads.operations + 1, TRUE, 1);
	TEST_CHECK_EQ('!', reads.bytes[1]);
	TEST_CHECK(!CancelIoEx((HANDLE)reads.socket, reads.operations));
	TEST_CHECK_EQ(ERROR_NOT_FOUND, GetLastError());
	queueRead(&reads, 2);
	queueRead(&reads, 3);
	TEST_CHECK(CancelIoEx((HANDLE)reads.socket, NULL));
	unsigned seen = 0;
	for (unsigned i = 0; i < 2; ++i) {
		OVERLAPPED *operation = completion(port, NULL, FALSE, 0);
		TEST_CHECK(operation == reads.operations + 2 || operation == reads.operations + 3);
		unsigned bit = operation == reads.operations + 2 ? 1 : 2;
		TEST_CHECK(!(seen & bit));
		seen |= bit;
	}
	TEST_CHECK_EQ(3, seen);
	TEST_CHECK(!CancelIoEx((HANDLE)reads.socket, NULL));
	TEST_CHECK_EQ(ERROR_NOT_FOUND, GetLastError());
	TEST_CHECK(!CancelIoEx(INVALID_HANDLE_VALUE, NULL));
	DWORD invalidError = GetLastError();
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, invalidError);
	else
		TEST_CHECK(invalidError == ERROR_INVALID_HANDLE || invalidError == ERROR_NOT_FOUND);
	TEST_CHECK(SetEvent(reads.release));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(reads.ready));
	TEST_CHECK(CloseHandle(reads.release));
	TEST_CHECK_EQ(0, closesocket(reads.socket));
	TEST_CHECK_EQ(0, closesocket(client));
	TEST_CHECK(CloseHandle(port));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
