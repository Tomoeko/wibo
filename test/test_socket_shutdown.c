#include <winsock2.h>

#include "test_assert.h"
#include <windows.h>

static char payloadByte(DWORD offset, DWORD length) { return (char)(offset * 17 + 11 + (offset >= length / 2)); }

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
	const DWORD receiveTimeout = 5000;
	TEST_CHECK_EQ(0,
				  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&receiveTimeout, sizeof(receiveTimeout)));
	TEST_CHECK_EQ(0,
				  setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, (const char *)&receiveTimeout, sizeof(receiveTimeout)));
	TEST_CHECK_EQ(SOCKET_ERROR, shutdown(INVALID_SOCKET, SD_BOTH));
	TEST_CHECK_EQ(WSAENOTSOCK, WSAGetLastError());
	TEST_CHECK_EQ(SOCKET_ERROR, shutdown(client, 3));
	TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
	const DWORD payloadLength = 524288;
	char *payload = (char *)malloc(payloadLength);
	TEST_CHECK(payload != NULL);
	for (DWORD i = 0; i < payloadLength; ++i)
		payload[i] = payloadByte(i, payloadLength);
	int smallBuffer = 4096;
	TEST_CHECK_EQ(0, setsockopt(client, SOL_SOCKET, SO_SNDBUF, (const char *)&smallBuffer, sizeof(smallBuffer)));
	OVERLAPPED writes[2] = {{0}, {0}};
	for (unsigned i = 0; i < 2; ++i) {
		WSABUF output = {payloadLength / 2, payload + i * payloadLength / 2};
		writes[i].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		TEST_CHECK(writes[i].hEvent != NULL);
		int issued = WSASend(client, &output, 1, NULL, 0, &writes[i], NULL);
		TEST_CHECK(issued == 0 || (issued == SOCKET_ERROR && WSAGetLastError() == ERROR_IO_PENDING));
	}
	typedef int(WSAAPI * ShutdownFunction)(SOCKET, int);
	FARPROC ordinal = GetProcAddress(GetModuleHandleA("ws2_32.dll"), (LPCSTR)22);
	ShutdownFunction closeSend;
	memcpy(&closeSend, &ordinal, sizeof(closeSend));
	TEST_CHECK(closeSend != NULL);
	TEST_CHECK_EQ(0, closeSend(client, SD_SEND));
	TEST_CHECK_EQ(SOCKET_ERROR, send(client, "!", 1, 0));
	TEST_CHECK_EQ(WSAESHUTDOWN, WSAGetLastError());
	char buffer[4096] = {0};
	DWORD offset = 0;
	while (offset < payloadLength) {
		int count = recv(server, buffer, sizeof(buffer), 0);
		TEST_CHECK(count > 0 && offset + (DWORD)count <= payloadLength);
		for (int i = 0; i < count; ++i)
			TEST_CHECK_EQ(payloadByte(offset + (DWORD)i, payloadLength), buffer[i]);
		offset += (DWORD)count;
	}
	TEST_CHECK_EQ(0, recv(server, buffer, sizeof(buffer), 0));
	DWORD bytes = 0, flags = 0;
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(writes[i].hEvent, 5000));
		TEST_CHECK(WSAGetOverlappedResult(client, &writes[i], &bytes, FALSE, &flags));
		TEST_CHECK_EQ(payloadLength / 2, bytes);
		TEST_CHECK_EQ(0, flags);
		TEST_CHECK(CloseHandle(writes[i].hEvent));
	}
	free(payload);
	TEST_CHECK_EQ(2, send(server, "ok", 2, 0));
	TEST_CHECK_EQ(2, recv(client, buffer, 2, MSG_WAITALL));
	TEST_CHECK(memcmp(buffer, "ok", 2) == 0);
	WSABUF input = {sizeof(buffer), buffer};
	OVERLAPPED pending = {0};
	pending.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(pending.hEvent != NULL);
	TEST_CHECK_EQ(SOCKET_ERROR, WSARecv(client, &input, 1, NULL, &flags, &pending, NULL));
	TEST_CHECK_EQ(ERROR_IO_PENDING, WSAGetLastError());
	TEST_CHECK_EQ(0, shutdown(server, SD_SEND));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(pending.hEvent, 5000));
	TEST_CHECK(WSAGetOverlappedResult(client, &pending, &bytes, FALSE, &flags));
	TEST_CHECK_EQ(0, bytes);
	TEST_CHECK_EQ(0, flags);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(pending.hEvent, 0));
	TEST_CHECK_EQ(0, shutdown(client, SD_RECEIVE));
	TEST_CHECK_EQ(SOCKET_ERROR, recv(client, buffer, sizeof(buffer), 0));
	TEST_CHECK_EQ(WSAESHUTDOWN, WSAGetLastError());
	TEST_CHECK_EQ(SOCKET_ERROR, WSARecv(client, &input, 1, &bytes, &flags, NULL, NULL));
	TEST_CHECK_EQ(WSAESHUTDOWN, WSAGetLastError());
	TEST_CHECK_EQ(SOCKET_ERROR, WSASend(client, &input, 1, &bytes, 0, NULL, NULL));
	TEST_CHECK_EQ(WSAESHUTDOWN, WSAGetLastError());
	TEST_CHECK(CloseHandle(pending.hEvent));
	TEST_CHECK_EQ(0, closesocket(client));
	TEST_CHECK_EQ(0, closesocket(server));
	SOCKET datagram = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	address.sin_port = 0;
	TEST_CHECK_EQ(0, bind(datagram, (const struct sockaddr *)&address, sizeof(address)));
	TEST_CHECK_EQ(0,
				  setsockopt(datagram, SOL_SOCKET, SO_RCVTIMEO, (const char *)&receiveTimeout, sizeof(receiveTimeout)));
	TEST_CHECK_EQ(0, shutdown(datagram, SD_BOTH));
	TEST_CHECK_EQ(SOCKET_ERROR, recvfrom(datagram, buffer, sizeof(buffer), 0, NULL, NULL));
	TEST_CHECK_EQ(WSAESHUTDOWN, WSAGetLastError());
	TEST_CHECK_EQ(0, closesocket(datagram));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
