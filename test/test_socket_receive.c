#include "test_assert.h"
#include <winsock2.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	char buffer[16] = {0};
	TEST_CHECK_EQ(SOCKET_ERROR, recv(INVALID_SOCKET, buffer, sizeof(buffer), 0));
	TEST_CHECK_EQ(WSAENOTSOCK, WSAGetLastError());
	SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	TEST_CHECK(handle != INVALID_SOCKET);
	u_long nonblocking = 1;
	TEST_CHECK_EQ(0, ioctlsocket(handle, FIONBIO, &nonblocking));
	TEST_CHECK_EQ(SOCKET_ERROR, recv(handle, buffer, sizeof(buffer), 0));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
	SOCKADDR_IN address = {0};
	address.sin_family = AF_INET;
	((BYTE *)&address.sin_addr)[0] = 127;
	((BYTE *)&address.sin_addr)[3] = 1;
	TEST_CHECK_EQ(0, bind(handle, (SOCKADDR *)&address, sizeof(address)));
	TEST_CHECK_EQ(SOCKET_ERROR, recv(handle, buffer, sizeof(buffer), 0));
	TEST_CHECK_EQ(WSAEWOULDBLOCK, WSAGetLastError());
	TEST_CHECK_EQ(SOCKET_ERROR, recvfrom(handle, buffer, sizeof(buffer), 0, NULL, NULL));
	TEST_CHECK_EQ(WSAEWOULDBLOCK, WSAGetLastError());
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		TEST_CHECK_EQ(SOCKET_ERROR, recv(handle, buffer, sizeof(buffer), MSG_WAITALL));
		TEST_CHECK_EQ(WSAEOPNOTSUPP, WSAGetLastError());
	}
	if (getenv("WIBO_TEST_DATAGRAM")) {
		int length = sizeof(address);
		TEST_CHECK_EQ(0, getsockname(handle, (SOCKADDR *)&address, &length));
		printf("PORT %u\n", ntohs(address.sin_port));
		fflush(stdout);
		nonblocking = 0;
		TEST_CHECK_EQ(0, ioctlsocket(handle, FIONBIO, &nonblocking));
		DWORD limit = 5000;
		TEST_CHECK_EQ(0, setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, (char *)&limit, sizeof(limit)));
		SOCKADDR_STORAGE sender = {0};
		int senderLength = sizeof(sender);
		TEST_CHECK_EQ(SOCKET_ERROR, recvfrom(handle, buffer, 2, MSG_PEEK, (SOCKADDR *)&sender, &senderLength));
		TEST_CHECK_EQ(WSAEMSGSIZE, WSAGetLastError());
		TEST_CHECK_EQ(sizeof(SOCKADDR_IN), senderLength);
		TEST_CHECK_EQ(AF_INET, sender.ss_family);
		TEST_CHECK(((SOCKADDR_IN *)&sender)->sin_port != 0);
		TEST_CHECK_EQ('a', buffer[0]);
		TEST_CHECK_EQ('b', buffer[1]);
		TEST_CHECK_EQ(6, recv(handle, buffer, sizeof(buffer), 0));
		TEST_CHECK_EQ(0, memcmp(buffer, "abcdef", 6));
		if (getenv("WIBO_FIXTURE_RUNTIME")) {
			senderLength = 1;
			TEST_CHECK_EQ(SOCKET_ERROR, recvfrom(handle, buffer, 2, 0, (SOCKADDR *)&sender, &senderLength));
			TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
		}
		senderLength = sizeof(sender);
		TEST_CHECK_EQ(SOCKET_ERROR, recvfrom(handle, buffer, 2, 0, (SOCKADDR *)&sender, &senderLength));
		TEST_CHECK_EQ(WSAEMSGSIZE, WSAGetLastError());
		TEST_CHECK_EQ(sizeof(SOCKADDR_IN), senderLength);
		TEST_CHECK_EQ('g', buffer[0]);
		TEST_CHECK_EQ('h', buffer[1]);
		TEST_CHECK_EQ(0, recv(handle, buffer, sizeof(buffer), 0));
	}
	nonblocking = 0;
	TEST_CHECK_EQ(0, ioctlsocket(handle, FIONBIO, &nonblocking));
	DWORD timeout = 20;
	TEST_CHECK_EQ(0, setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)));
	TEST_CHECK_EQ(SOCKET_ERROR, recv(handle, buffer, sizeof(buffer), 0));
	TEST_CHECK_EQ(WSAETIMEDOUT, WSAGetLastError());
	TEST_CHECK_EQ(0, closesocket(handle));
	const char *port = getenv("WIBO_TEST_SERVER_PORT");
	if (port) {
		handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		TEST_CHECK(handle != INVALID_SOCKET);
		address.sin_port = htons((u_short)atoi(port));
		TEST_CHECK_EQ(0, connect(handle, (SOCKADDR *)&address, sizeof(address)));
		if (getenv("WIBO_FIXTURE_RUNTIME")) {
			TEST_CHECK_EQ(SOCKET_ERROR, recv(handle, buffer, 0, 0));
			TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
		}
		TEST_CHECK_EQ(SOCKET_ERROR, recv(handle, NULL, 1, 0));
		TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
		if (getenv("WIBO_FIXTURE_RUNTIME")) {
			TEST_CHECK_EQ(SOCKET_ERROR, recv(handle, buffer, sizeof(buffer), MSG_WAITALL | MSG_PEEK));
			TEST_CHECK_EQ(WSAEOPNOTSUPP, WSAGetLastError());
		}
		TEST_CHECK_EQ(3, recv(handle, buffer, 3, MSG_PEEK));
		TEST_CHECK_EQ(0, memcmp(buffer, "abc", 3));
		int received = recv(handle, buffer, 6, MSG_WAITALL);
		if (getenv("WIBO_FIXTURE_RUNTIME"))
			TEST_CHECK_EQ(6, received);
		else {
			TEST_CHECK(received > 0 && received <= 6);
			while (received < 6) {
				int next = recv(handle, buffer + received, 6 - received, 0);
				TEST_CHECK(next > 0);
				received += next;
			}
		}
		TEST_CHECK_EQ(0, memcmp(buffer, "abcdef", 6));
		TEST_CHECK_EQ(0, recv(handle, buffer, sizeof(buffer), 0));
		TEST_CHECK_EQ(0, recvfrom(handle, buffer, sizeof(buffer), 0, NULL, NULL));
		if (getenv("WIBO_FIXTURE_RUNTIME")) {
			HMODULE library = GetModuleHandleA("ws2_32.dll");
			int(WSAAPI * ordinalReceive)(SOCKET, char *, int, int) = (void *)GetProcAddress(library, (LPCSTR)16);
			TEST_CHECK(ordinalReceive != NULL);
			TEST_CHECK_EQ(0, ordinalReceive(handle, buffer, sizeof(buffer), 0));
		}
		TEST_CHECK_EQ(0, closesocket(handle));
	}
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
