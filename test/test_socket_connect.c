#include "test_assert.h"
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	for (int familyIndex = 0; familyIndex != 2; ++familyIndex) {
		int family = familyIndex ? AF_INET6 : AF_INET;
		SOCKET handle = socket(family, SOCK_DGRAM, IPPROTO_UDP);
		TEST_CHECK(handle != INVALID_SOCKET);
		SOCKADDR_STORAGE address = {0};
		address.ss_family = family;
		int size = familyIndex ? sizeof(SOCKADDR_IN6) : sizeof(SOCKADDR_IN);
		((SOCKADDR_IN *)&address)->sin_port = htons(1);
		if (familyIndex)
			((SOCKADDR_IN6 *)&address)->sin6_addr.u.Byte[15] = 1;
		else {
			BYTE *bytes = (BYTE *)&((SOCKADDR_IN *)&address)->sin_addr;
			bytes[0] = 127;
			bytes[3] = 1;
		}
		TEST_CHECK_EQ(SOCKET_ERROR, connect(handle, (SOCKADDR *)&address, 1));
		if (getenv("WIBO_FIXTURE_RUNTIME"))
			TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
		TEST_CHECK_EQ(0, connect(handle, (SOCKADDR *)&address, size));
		SOCKADDR_STORAGE local = {0};
		int length = sizeof(local);
		TEST_CHECK_EQ(0, getsockname(handle, (SOCKADDR *)&local, &length));
		TEST_CHECK_EQ(size, length);
		TEST_CHECK(((SOCKADDR_IN *)&local)->sin_port != 0);
		((SOCKADDR_IN *)&address)->sin_port = htons(2);
		TEST_CHECK_EQ(0, connect(handle, (SOCKADDR *)&address, size));
		TEST_CHECK_EQ(0, closesocket(handle));
	}
	SOCKET reserved = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK(reserved != INVALID_SOCKET && client != INVALID_SOCKET);
	SOCKADDR_IN destination = {0};
	destination.sin_family = AF_INET;
	BYTE *bytes = (BYTE *)&destination.sin_addr;
	bytes[0] = 127;
	bytes[3] = 1;
	TEST_CHECK_EQ(0, bind(reserved, (SOCKADDR *)&destination, sizeof(destination)));
	int length = sizeof(destination);
	TEST_CHECK_EQ(0, getsockname(reserved, (SOCKADDR *)&destination, &length));
	TEST_CHECK_EQ(0, closesocket(reserved));
	TEST_CHECK_EQ(SOCKET_ERROR, connect(client, (SOCKADDR *)&destination, length));
	TEST_CHECK_EQ(WSAECONNREFUSED, WSAGetLastError());
	HMODULE library = GetModuleHandleA("ws2_32.dll");
	int(WSAAPI * ordinalError)(void) = (void *)GetProcAddress(library, (LPCSTR)111);
	TEST_CHECK(ordinalError != NULL);
	TEST_CHECK_EQ(WSAECONNREFUSED, ordinalError());
	TEST_CHECK_EQ(0, closesocket(client));
	const char *port = getenv("WIBO_TEST_SERVER_PORT");
	if (port) {
		client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		TEST_CHECK(client != INVALID_SOCKET);
		destination.sin_port = htons((u_short)atoi(port));
		TEST_CHECK_EQ(0, connect(client, (SOCKADDR *)&destination, sizeof(destination)));
		TEST_CHECK_EQ(0, closesocket(client));
	}
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
