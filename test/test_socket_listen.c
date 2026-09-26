#include "test_assert.h"
#include <winsock2.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK(handle != INVALID_SOCKET);
	TEST_CHECK_EQ(SOCKET_ERROR, listen(handle, 4));
	TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
	SOCKADDR_IN address = {0};
	address.sin_family = AF_INET;
	((BYTE *)&address.sin_addr)[0] = 127;
	((BYTE *)&address.sin_addr)[3] = 1;
	TEST_CHECK_EQ(0, bind(handle, (SOCKADDR *)&address, sizeof(address)));
	int accepting = -1, length = sizeof(accepting);
	TEST_CHECK_EQ(0, getsockopt(handle, SOL_SOCKET, SO_ACCEPTCONN, (char *)&accepting, &length));
	TEST_CHECK_EQ(0, accepting);
	TEST_CHECK_EQ(0, listen(handle, 4));
	TEST_CHECK_EQ(0, getsockopt(handle, SOL_SOCKET, SO_ACCEPTCONN, (char *)&accepting, &length));
	TEST_CHECK(accepting != 0);
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		HMODULE module = GetModuleHandleA("ws2_32.dll");
		int(WSAAPI * byOrdinal)(SOCKET, int) = (void *)GetProcAddress(module, (LPCSTR)13);
		TEST_CHECK(byOrdinal != NULL);
		TEST_CHECK_EQ(0, byOrdinal(handle, 8));
	}
	if (getenv("WIBO_TEST_SERVER")) {
		length = sizeof(address);
		TEST_CHECK_EQ(0, getsockname(handle, (SOCKADDR *)&address, &length));
		printf("PORT %u\n", ntohs(address.sin_port));
		fflush(stdout);
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(handle, &readable);
		struct timeval timeout = {5, 0};
		TEST_CHECK_EQ(1, select(0, &readable, NULL, NULL, &timeout));
		TEST_CHECK_EQ(1, readable.fd_count);
	}
	TEST_CHECK_EQ(0, closesocket(handle));
	TEST_CHECK_EQ(SOCKET_ERROR, listen(handle, 4));
	TEST_CHECK_EQ(WSAENOTSOCK, WSAGetLastError());
	handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	TEST_CHECK(handle != INVALID_SOCKET);
	TEST_CHECK_EQ(0, bind(handle, (SOCKADDR *)&address, sizeof(address)));
	TEST_CHECK_EQ(SOCKET_ERROR, listen(handle, 4));
	TEST_CHECK_EQ(WSAEOPNOTSUPP, WSAGetLastError());
	TEST_CHECK_EQ(0, closesocket(handle));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
