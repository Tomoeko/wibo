#include <winsock2.h>
#include <ws2tcpip.h>

#include "test_assert.h"
#include <windows.h>

static void checkFamily(int family) {
	struct sockaddr_storage address = {0};
	int addressLength;
	if (family == AF_INET) {
		struct sockaddr_in *ip = (struct sockaddr_in *)&address;
		ip->sin_family = AF_INET;
		ip->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addressLength = sizeof(*ip);
	} else {
		struct sockaddr_in6 *ip = (struct sockaddr_in6 *)&address;
		ip->sin6_family = AF_INET6;
		ip->sin6_addr.s6_addr[15] = 1;
		addressLength = sizeof(*ip);
	}
	SOCKET listener = socket(family, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK(listener != INVALID_SOCKET);
	TEST_CHECK_EQ(0, bind(listener, (struct sockaddr *)&address, addressLength));
	TEST_CHECK_EQ(0, listen(listener, 1));
	int length = addressLength;
	TEST_CHECK_EQ(0, getsockname(listener, (struct sockaddr *)&address, &length));
	struct sockaddr_storage peer;
	length = sizeof(peer);
	TEST_CHECK_EQ(SOCKET_ERROR, getpeername(listener, (struct sockaddr *)&peer, &length));
	TEST_CHECK_EQ(WSAENOTCONN, WSAGetLastError());
	SOCKET client = socket(family, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK_EQ(0, connect(client, (struct sockaddr *)&address, addressLength));
	SOCKET server = accept(listener, NULL, NULL);
	TEST_CHECK(server != INVALID_SOCKET);
	memset(&peer, 0xCC, sizeof(peer));
	length = sizeof(peer);
	typedef int(WSAAPI * PeerFunction)(SOCKET, struct sockaddr *, int *);
	FARPROC ordinal = GetProcAddress(GetModuleHandleA("ws2_32.dll"), (LPCSTR)5);
	PeerFunction query;
	memcpy(&query, &ordinal, sizeof(query));
	TEST_CHECK(query != NULL);
	TEST_CHECK_EQ(0, query(client, (struct sockaddr *)&peer, &length));
	TEST_CHECK_EQ(addressLength, length);
	TEST_CHECK(memcmp(&address, &peer, addressLength) == 0);
	TEST_CHECK_EQ(0xCC, ((unsigned char *)&peer)[addressLength]);
	struct sockaddr_storage local = {0};
	length = sizeof(local);
	TEST_CHECK_EQ(0, getsockname(client, (struct sockaddr *)&local, &length));
	length = sizeof(peer);
	TEST_CHECK_EQ(0, getpeername(server, (struct sockaddr *)&peer, &length));
	TEST_CHECK_EQ(addressLength, length);
	TEST_CHECK(memcmp(&local, &peer, addressLength) == 0);
	length = addressLength - 1;
	TEST_CHECK_EQ(SOCKET_ERROR, getpeername(client, (struct sockaddr *)&peer, &length));
	TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
	TEST_CHECK_EQ(SOCKET_ERROR, getpeername(client, NULL, &length));
	int nullError = WSAGetLastError();
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(WSAEFAULT, nullError);
	else
		TEST_CHECK(nullError == WSAEFAULT || nullError == WSAEINVAL);
	TEST_CHECK_EQ(0, closesocket(client));
	TEST_CHECK_EQ(0, closesocket(server));
	TEST_CHECK_EQ(0, closesocket(listener));
}

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	checkFamily(AF_INET);
	checkFamily(AF_INET6);
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
