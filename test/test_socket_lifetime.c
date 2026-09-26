#include "test_assert.h"
#include <winsock2.h>
#include <ws2tcpip.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	for (int caseIndex = 0; caseIndex != 4; ++caseIndex) {
		int family = (caseIndex % 2) ? AF_INET6 : AF_INET;
		SOCKET first = caseIndex < 2 ? WSASocketA(family, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED)
									   : WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
		TEST_CHECK(first != INVALID_SOCKET);
		SOCKADDR_STORAGE address;
		memset(&address, 0, sizeof(address));
		address.ss_family = family;
		int size = (caseIndex % 2) ? sizeof(SOCKADDR_IN6) : sizeof(SOCKADDR_IN);
		if (caseIndex % 2)
			((SOCKADDR_IN6 *)&address)->sin6_addr.u.Byte[15] = 1;
		else {
			BYTE *bytes = (BYTE *)&((SOCKADDR_IN *)&address)->sin_addr;
			bytes[0] = 127;
			bytes[3] = 1;
		}
		TEST_CHECK_EQ(0, bind(first, (SOCKADDR *)&address, size));
		int length = sizeof(address);
		TEST_CHECK_EQ(0, getsockname(first, (SOCKADDR *)&address, &length));
		TEST_CHECK_EQ(size, length);
		TEST_CHECK_EQ(family, address.ss_family);
		TEST_CHECK(((SOCKADDR_IN *)&address)->sin_port != 0);
		SOCKET second = socket(family, SOCK_DGRAM, IPPROTO_UDP);
		TEST_CHECK(second != INVALID_SOCKET);
		TEST_CHECK_EQ(SOCKET_ERROR, bind(second, (SOCKADDR *)&address, size));
		TEST_CHECK_EQ(WSAEADDRINUSE, WSAGetLastError());
		TEST_CHECK_EQ(0, closesocket(first));
		TEST_CHECK_EQ(0, bind(second, (SOCKADDR *)&address, size));
		TEST_CHECK_EQ(0, closesocket(second));
		TEST_CHECK_EQ(SOCKET_ERROR, closesocket(second));
		TEST_CHECK_EQ(WSAENOTSOCK, WSAGetLastError());
	}
	SOCKET pending = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	TEST_CHECK(pending != INVALID_SOCKET);
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	TEST_CHECK_EQ(0, WSACleanup());
	SOCKADDR_IN local = {0};
	local.sin_family = AF_INET;
	TEST_CHECK_EQ(0, bind(pending, (SOCKADDR *)&local, sizeof(local)));
	TEST_CHECK_EQ(0, WSACleanup());
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	TEST_CHECK_EQ(SOCKET_ERROR, closesocket(pending));
	TEST_CHECK_EQ(WSAENOTSOCK, WSAGetLastError());
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
