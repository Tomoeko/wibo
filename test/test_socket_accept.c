#include "test_assert.h"
#include <winsock2.h>
#include <ws2tcpip.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	for (int mode = 0; mode != 2; ++mode) {
		int family = mode ? AF_INET6 : AF_INET;
		SOCKET listener = socket(family, SOCK_STREAM, IPPROTO_TCP), client = socket(family, SOCK_STREAM, IPPROTO_TCP);
		TEST_CHECK(listener != INVALID_SOCKET && client != INVALID_SOCKET);
		SOCKADDR_STORAGE address = {0}, peer = {0};
		address.ss_family = family;
		if (mode)
			((SOCKADDR_IN6 *)&address)->sin6_addr.u.Byte[15] = 1;
		else {
			((BYTE *)&((SOCKADDR_IN *)&address)->sin_addr)[0] = 127;
			((BYTE *)&((SOCKADDR_IN *)&address)->sin_addr)[3] = 1;
		}
		int length = mode ? sizeof(SOCKADDR_IN6) : sizeof(SOCKADDR_IN);
		TEST_CHECK_EQ(0, bind(listener, (SOCKADDR *)&address, length));
		TEST_CHECK_EQ(0, getsockname(listener, (SOCKADDR *)&address, &length));
		TEST_CHECK_EQ(0, listen(listener, 4));
		u_long nonblocking = 1;
		TEST_CHECK_EQ(0, ioctlsocket(listener, FIONBIO, &nonblocking));
		TEST_CHECK_EQ(INVALID_SOCKET, accept(listener, NULL, NULL));
		TEST_CHECK_EQ(WSAEWOULDBLOCK, WSAGetLastError());
		int keepAlive = 1;
		TEST_CHECK_EQ(0, setsockopt(listener, SOL_SOCKET, SO_KEEPALIVE, (char *)&keepAlive, sizeof(keepAlive)));
		TEST_CHECK_EQ(0, connect(client, (SOCKADDR *)&address, length));
		int peerLength = sizeof(peer);
		SOCKET accepted = accept(listener, (SOCKADDR *)&peer, &peerLength);
		TEST_CHECK(accepted != INVALID_SOCKET && accepted != listener && accepted != client);
		TEST_CHECK_EQ(length, peerLength);
		TEST_CHECK_EQ(family, peer.ss_family);
		TEST_CHECK(((SOCKADDR_IN *)&peer)->sin_port != 0);
		char byte = 0;
		TEST_CHECK_EQ(SOCKET_ERROR, recv(accepted, &byte, 1, 0));
		TEST_CHECK_EQ(WSAEWOULDBLOCK, WSAGetLastError());
		int optionLength = sizeof(keepAlive);
		keepAlive = 0;
		TEST_CHECK_EQ(0, getsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, (char *)&keepAlive, &optionLength));
		TEST_CHECK(keepAlive != 0);
		TEST_CHECK_EQ(0, closesocket(listener));
		TEST_CHECK_EQ(SOCKET_ERROR, recv(accepted, &byte, 1, 0));
		TEST_CHECK_EQ(WSAEWOULDBLOCK, WSAGetLastError());
		TEST_CHECK_EQ(0, closesocket(client));
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(accepted, &readable);
		struct timeval timeout = {5, 0};
		TEST_CHECK_EQ(1, select(0, &readable, NULL, NULL, &timeout));
		TEST_CHECK_EQ(0, recv(accepted, &byte, 1, 0));
		TEST_CHECK_EQ(0, closesocket(accepted));
	}
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
