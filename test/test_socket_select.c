#include "test_assert.h"
#include <winsock2.h>

int main(void) {
	TEST_CHECK_EQ(8, sizeof(struct timeval));
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	TEST_CHECK(handle != INVALID_SOCKET);
	SOCKADDR_IN address = {0};
	address.sin_family = AF_INET;
	((BYTE *)&address.sin_addr)[0] = 127;
	((BYTE *)&address.sin_addr)[3] = 1;
	TEST_CHECK_EQ(0, bind(handle, (SOCKADDR *)&address, sizeof(address)));
	fd_set readSet, writeSet, exceptSet;
	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);
	FD_ZERO(&exceptSet);
	FD_SET(handle, &readSet);
	FD_SET(handle, &writeSet);
	struct timeval timeout = {0, 0};
	TEST_CHECK_EQ(1, select(-1, &readSet, &writeSet, &exceptSet, &timeout));
	TEST_CHECK_EQ(0, readSet.fd_count);
	TEST_CHECK_EQ(1, writeSet.fd_count);
	TEST_CHECK_EQ(handle, writeSet.fd_array[0]);
	FD_SET(handle, &readSet);
	timeout.tv_usec = 20000;
	DWORD start = GetTickCount();
	TEST_CHECK_EQ(0, select(0, &readSet, NULL, NULL, &timeout));
	TEST_CHECK(GetTickCount() - start >= 10);
	TEST_CHECK_EQ(0, readSet.fd_count);
	TEST_CHECK_EQ(0, timeout.tv_sec);
	TEST_CHECK_EQ(20000, timeout.tv_usec);
	TEST_CHECK_EQ(SOCKET_ERROR, select(0, NULL, NULL, NULL, &timeout));
	TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
	FD_SET(handle, &readSet);
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		timeout.tv_sec = -1;
		TEST_CHECK_EQ(SOCKET_ERROR, select(0, &readSet, NULL, NULL, &timeout));
		TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
		TEST_CHECK_EQ(1, readSet.fd_count);
	}
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	if (getenv("WIBO_TEST_DATAGRAM")) {
		int length = sizeof(address);
		TEST_CHECK_EQ(0, getsockname(handle, (SOCKADDR *)&address, &length));
		printf("PORT %u\n", ntohs(address.sin_port));
		fflush(stdout);
		timeout.tv_sec = 5;
		TEST_CHECK_EQ(1, select(0, &readSet, NULL, NULL, &timeout));
		TEST_CHECK_EQ(1, readSet.fd_count);
		TEST_CHECK_EQ(handle, readSet.fd_array[0]);
		FD_SET(handle, &writeSet);
		timeout.tv_sec = 0;
		TEST_CHECK_EQ(2, select(0, &readSet, &writeSet, NULL, &timeout));
	}
	TEST_CHECK_EQ(0, closesocket(handle));
	FD_ZERO(&readSet);
	FD_SET(handle, &readSet);
	TEST_CHECK_EQ(SOCKET_ERROR, select(0, &readSet, NULL, NULL, &timeout));
	TEST_CHECK_EQ(WSAENOTSOCK, WSAGetLastError());
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
