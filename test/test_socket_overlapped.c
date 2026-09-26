#include <winsock2.h>

#include "test_assert.h"
#include <windows.h>

static void completions(HANDLE port, OVERLAPPED *read, DWORD readBytes, OVERLAPPED *write, DWORD writeBytes) {
	BOOL readSeen = FALSE, writeSeen = FALSE;
	for (unsigned i = 0; i < 2; ++i) {
		DWORD bytes = 0;
		ULONG_PTR key = 0;
		OVERLAPPED *operation = NULL;
		TEST_CHECK(GetQueuedCompletionStatus(port, &bytes, &key, &operation, 5000));
		if (operation == read) {
			TEST_CHECK(!readSeen);
			readSeen = TRUE;
			TEST_CHECK_EQ(0x71, key);
			TEST_CHECK_EQ(readBytes, bytes);
		} else {
			TEST_CHECK(operation == write && !writeSeen);
			writeSeen = TRUE;
			TEST_CHECK_EQ(0x81, key);
			TEST_CHECK_EQ(writeBytes, bytes);
		}
	}
	TEST_CHECK(readSeen && writeSeen);
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
	SOCKET server = accept(listener, NULL, NULL);
	TEST_CHECK(server != INVALID_SOCKET);
	TEST_CHECK_EQ(0, closesocket(listener));
	HANDLE port = CreateIoCompletionPort((HANDLE)server, NULL, 0x71, 1);
	TEST_CHECK(port != NULL);
	TEST_CHECK(CreateIoCompletionPort((HANDLE)client, port, 0x81, 0) == port);
	char first[4] = "???", second[5] = "????", discarded[8] = {0};
	WSABUF input[2] = {{2, first + 1}, {4, second}};
	OVERLAPPED read = {0}, write = {0};
	DWORD flags = 0;
	TEST_CHECK_EQ(SOCKET_ERROR, WSARecv(server, input, 2, NULL, &flags, &read, NULL));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	input[0].buf = discarded;
	input[1].buf = discarded;
	WSABUF output[2] = {{2, "he"}, {3, "llo"}};
	int issued = WSASend(client, output, 2, NULL, 0, &write, NULL);
	TEST_CHECK(issued == 0 || (issued == SOCKET_ERROR && WSAGetLastError() == ERROR_IO_PENDING));
	completions(port, &read, 5, &write, 5);
	TEST_CHECK(memcmp(first + 1, "he", 2) == 0);
	TEST_CHECK_EQ('?', first[0]);
	TEST_CHECK_EQ(0, first[3]);
	TEST_CHECK(memcmp(second, "llo", 3) == 0);
	TEST_CHECK_EQ('?', second[3]);
	TEST_CHECK_EQ(0, discarded[0]);
	DWORD transferred = 0;
	TEST_CHECK(WSAGetOverlappedResult(server, &read, &transferred, FALSE, &flags));
	TEST_CHECK_EQ(5, transferred);
	TEST_CHECK_EQ(0, flags);
	WSABUF zero = {0, NULL};
	OVERLAPPED readiness = {0}, sendByte = {0};
	TEST_CHECK_EQ(SOCKET_ERROR, WSARecv(server, &zero, 1, NULL, &flags, &readiness, NULL));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	OVERLAPPED *completed = NULL;
	ULONG_PTR key = 0;
	TEST_CHECK(!GetQueuedCompletionStatus(port, &transferred, &key, &completed, 0));
	TEST_CHECK_EQ(WAIT_TIMEOUT, GetLastError());
	WSABUF one = {1, "!"};
	issued = WSASend(client, &one, 1, NULL, 0, &sendByte, NULL);
	TEST_CHECK(issued == 0 || (issued == SOCKET_ERROR && WSAGetLastError() == ERROR_IO_PENDING));
	completions(port, &readiness, 0, &sendByte, 1);
	char byte = 0;
	WSABUF destination = {1, &byte};
	TEST_CHECK_EQ(0, WSARecv(server, &destination, 1, &transferred, &flags, NULL, NULL));
	TEST_CHECK_EQ(1, transferred);
	TEST_CHECK_EQ('!', byte);
	TEST_CHECK_EQ(1, send(client, "?", 1, MSG_DONTROUTE));
	TEST_CHECK_EQ(0, WSARecv(server, &destination, 1, &transferred, &flags, NULL, NULL));
	TEST_CHECK_EQ('?', byte);
	const DWORD largeLength = 262144;
	char *large = (char *)malloc(largeLength);
	TEST_CHECK(large != NULL);
	for (DWORD i = 0; i < largeLength; ++i)
		large[i] = (char)(i * 17 + 11);
	int smallBuffer = 4096;
	TEST_CHECK_EQ(0, setsockopt(client, SOL_SOCKET, SO_SNDBUF, (const char *)&smallBuffer, sizeof(smallBuffer)));
	WSABUF largeOutput[2] = {{131073, large}, {largeLength - 131073, large + 131073}};
	OVERLAPPED largeWrite = {0};
	issued = WSASend(client, largeOutput, 2, NULL, 0, &largeWrite, NULL);
	TEST_CHECK(issued == 0 || (issued == SOCKET_ERROR && WSAGetLastError() == ERROR_IO_PENDING));
	largeOutput[0].len = largeOutput[1].len = 0;
	char chunk[4096];
	DWORD offset = 0;
	while (offset < largeLength) {
		WSABUF readChunk = {sizeof(chunk), chunk};
		TEST_CHECK_EQ(0, WSARecv(server, &readChunk, 1, &transferred, &flags, NULL, NULL));
		TEST_CHECK(transferred > 0 && transferred <= sizeof(chunk));
		TEST_CHECK(offset + transferred <= largeLength);
		TEST_CHECK(memcmp(chunk, large + offset, transferred) == 0);
		offset += transferred;
	}
	TEST_CHECK(GetQueuedCompletionStatus(port, &transferred, &key, &completed, 5000));
	TEST_CHECK_EQ(0x81, key);
	TEST_CHECK(completed == &largeWrite);
	TEST_CHECK_EQ(largeLength, transferred);
	free(large);
	OVERLAPPED cancelled = {0};
	TEST_CHECK_EQ(SOCKET_ERROR, WSARecv(server, &destination, 1, NULL, &flags, &cancelled, NULL));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	TEST_CHECK_EQ(0, closesocket(server));
	TEST_CHECK(!GetQueuedCompletionStatus(port, &transferred, &key, &completed, 5000));
	DWORD cancellationError = GetLastError();
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(ERROR_OPERATION_ABORTED, cancellationError);
	else
		TEST_CHECK(cancellationError == ERROR_OPERATION_ABORTED || cancellationError == ERROR_HANDLES_CLOSED);
	TEST_CHECK_EQ(0x71, key);
	TEST_CHECK(completed == &cancelled);
	TEST_CHECK_EQ(0, closesocket(client));
	TEST_CHECK(CloseHandle(port));
	SOCKET datagramServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	address.sin_port = 0;
	TEST_CHECK_EQ(0, bind(datagramServer, (const struct sockaddr *)&address, sizeof(address)));
	length = sizeof(address);
	TEST_CHECK_EQ(0, getsockname(datagramServer, (struct sockaddr *)&address, &length));
	SOCKET datagramClient = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	TEST_CHECK_EQ(0, connect(datagramClient, (const struct sockaddr *)&address, sizeof(address)));
	port = CreateIoCompletionPort((HANDLE)datagramServer, NULL, 0x71, 1);
	TEST_CHECK(port != NULL);
	TEST_CHECK(CreateIoCompletionPort((HANDLE)datagramClient, port, 0x81, 0) == port);
	char truncated[5] = "????";
	WSABUF shortInput = {3, truncated + 1};
	WSABUF longOutput = {5, "hello"};
	OVERLAPPED datagramRead = {0}, datagramWrite = {0};
	TEST_CHECK_EQ(SOCKET_ERROR, WSARecv(datagramServer, &shortInput, 1, NULL, &flags, &datagramRead, NULL));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	issued = WSASend(datagramClient, &longOutput, 1, NULL, 0, &datagramWrite, NULL);
	TEST_CHECK(issued == 0 || (issued == SOCKET_ERROR && WSAGetLastError() == ERROR_IO_PENDING));
	for (unsigned i = 0; i < 2; ++i) {
		BOOL success = GetQueuedCompletionStatus(port, &transferred, &key, &completed, 5000);
		if (completed == &datagramRead) {
			TEST_CHECK(!success);
			TEST_CHECK_EQ(ERROR_MORE_DATA, GetLastError());
			TEST_CHECK_EQ(3, transferred);
		} else {
			TEST_CHECK(success && completed == &datagramWrite);
			TEST_CHECK_EQ(5, transferred);
		}
	}
	TEST_CHECK(!WSAGetOverlappedResult(datagramServer, &datagramRead, &transferred, FALSE, &flags));
	TEST_CHECK_EQ(WSAEMSGSIZE, WSAGetLastError());
	TEST_CHECK(memcmp(truncated + 1, "hel", 3) == 0);
	TEST_CHECK_EQ('?', truncated[0]);
	TEST_CHECK_EQ(0, truncated[4]);
	TEST_CHECK_EQ(0, closesocket(datagramClient));
	TEST_CHECK_EQ(0, closesocket(datagramServer));
	TEST_CHECK(CloseHandle(port));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
