#include "test_assert.h"
#include <winsock2.h>
#include <ws2tcpip.h>

static void integer_option(SOCKET handle, int level, int name, int value) {
	TEST_CHECK_EQ(0, setsockopt(handle, level, name, (const char *)&value, sizeof(value)));
	int result = 0, size = sizeof(result);
	TEST_CHECK_EQ(0, getsockopt(handle, level, name, (char *)&result, &size));
	TEST_CHECK(size > 0 && size <= sizeof(result));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(sizeof(result), size);
	if (value == 0 || value == 1)
		TEST_CHECK_EQ(value, result != 0);
	else
		TEST_CHECK_EQ(value, result);
}

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	TEST_CHECK(tcp != INVALID_SOCKET);
	integer_option(tcp, SOL_SOCKET, SO_KEEPALIVE, 1);
	integer_option(tcp, SOL_SOCKET, SO_KEEPALIVE, 0);
	integer_option(tcp, IPPROTO_TCP, TCP_NODELAY, 1);
	integer_option(tcp, IPPROTO_TCP, TCP_NODELAY, 0);
	integer_option(tcp, SOL_SOCKET, SO_RCVTIMEO, 1000);
	LINGER setting = {1, 7}, result = {0};
	TEST_CHECK_EQ(0, setsockopt(tcp, SOL_SOCKET, SO_LINGER, (const char *)&setting, sizeof(setting)));
	int size = sizeof(result);
	TEST_CHECK_EQ(0, getsockopt(tcp, SOL_SOCKET, SO_LINGER, (char *)&result, &size));
	TEST_CHECK_EQ(sizeof(result), size);
	TEST_CHECK_EQ(1, result.l_onoff);
	TEST_CHECK_EQ(7, result.l_linger);
	struct {
		DWORD before;
		u_long count;
		DWORD after;
	} available = {0x12345678, 0xFFFFFFFF, 0x23456789};
	TEST_CHECK_EQ(0, ioctlsocket(tcp, FIONREAD, &available.count));
	TEST_CHECK_EQ(0x12345678, available.before);
	TEST_CHECK_EQ(0x23456789, available.after);
	TEST_CHECK_EQ(0, available.count);
	available.count = 1;
	TEST_CHECK_EQ(0, ioctlsocket(tcp, FIONBIO, &available.count));
	available.count = 0;
	TEST_CHECK_EQ(0, ioctlsocket(tcp, FIONBIO, &available.count));
	TEST_CHECK_EQ(0, closesocket(tcp));
	SOCKET ipv6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	TEST_CHECK(ipv6 != INVALID_SOCKET);
	int enabled = 0;
	size = sizeof(enabled);
	TEST_CHECK_EQ(0, getsockopt(ipv6, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&enabled, &size));
	TEST_CHECK_EQ(1, enabled);
	integer_option(ipv6, IPPROTO_IPV6, IPV6_V6ONLY, 0);
	integer_option(ipv6, IPPROTO_IPV6, IPV6_V6ONLY, 1);
	integer_option(ipv6, SOL_SOCKET, SO_BROADCAST, 1);
	TEST_CHECK_EQ(0, closesocket(ipv6));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
