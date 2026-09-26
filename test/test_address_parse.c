#include "test_assert.h"
#include <winsock2.h>
#include <ws2tcpip.h>

static void ipv4(WCHAR *text, DWORD expected, WORD port) {
	union {
		SOCKADDR_STORAGE address;
		BYTE bytes[132];
	} buffer;
	memset(&buffer, 0xa5, sizeof(buffer));
	int size = sizeof(buffer);
	WSASetLastError(0x1234);
	const int status = WSAStringToAddressW(text, AF_INET, NULL, (SOCKADDR *)&buffer, &size);
	TEST_CHECK_MSG(status == 0, "parse '%ls' failed: %d length=%d", text, WSAGetLastError(), size);
	TEST_CHECK_EQ(0x1234, WSAGetLastError());
	TEST_CHECK_EQ(sizeof(SOCKADDR_IN), size);
	SOCKADDR_IN *result = (SOCKADDR_IN *)&buffer;
	TEST_CHECK_EQ(AF_INET, result->sin_family);
	TEST_CHECK_EQ(port, ntohs(result->sin_port));
	TEST_CHECK_EQ(expected, result->sin_addr.S_un.S_addr);
	for (unsigned index = 8; index < sizeof(SOCKADDR_IN); ++index)
		TEST_CHECK_EQ(0, buffer.bytes[index]);
	for (unsigned index = sizeof(SOCKADDR_IN); index < sizeof(buffer); ++index)
		TEST_CHECK_EQ(0xa5, buffer.bytes[index]);
}

static void ipv6(WCHAR *text, const BYTE expected[16], WORD port, DWORD scope) {
	SOCKADDR_IN6 result;
	memset(&result, 0xa5, sizeof(result));
	int size = sizeof(result);
	WSASetLastError(0x1234);
	TEST_CHECK_EQ(0, WSAStringToAddressW(text, AF_INET6, NULL, (SOCKADDR *)&result, &size));
	TEST_CHECK_EQ(0x1234, WSAGetLastError());
	TEST_CHECK_EQ(sizeof(result), size);
	TEST_CHECK_EQ(AF_INET6, result.sin6_family);
	TEST_CHECK_EQ(port, ntohs(result.sin6_port));
	TEST_CHECK_EQ(0, result.sin6_flowinfo);
	TEST_CHECK_EQ(scope, result.sin6_scope_id);
	TEST_CHECK(memcmp(&result.sin6_addr, expected, 16) == 0);
}

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	ipv4(L"127.0.0.1", 0x0100007f, 0);
	ipv4(L"127.0.0.1:443", 0x0100007f, 443);
	ipv4(L"192.0.2.1:65535", 0x010200c0, 65535);
	ipv4(L"255.255.255.255", 0xffffffff, 0);
	ipv4(L"0.0.0.0", 0, 0);
	ipv4(L"127.1", 0x0100007f, 0);
	ipv4(L"0177.0.0.1", 0x0100007f, 0);
	ipv4(L"0x7f000001", 0x0100007f, 0);
	const BYTE loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	const BYTE linklocal[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
	const BYTE mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1};
	ipv6(L"::1", loopback, 0, 0);
	ipv6(L"[::1]", loopback, 0, 0);
	ipv6(L"[::1]:443", loopback, 443, 0);
	ipv6(L"[fe80::1%3]:7", linklocal, 7, 3);
	ipv6(L"fe80::1%3", linklocal, 0, 3);
	ipv6(L"::ffff:192.0.2.1", mapped, 0, 0);
	WCHAR *invalid4[] = {L"",		  L"1.2.3.4:65536", L"1.2.3.4:0", L"1.2.3.4:-1",
						 L"1.2.3.4:", L"256.0.0.1",		L"1..2",	  L"invalid"};
	for (unsigned index = 0; index < sizeof(invalid4) / sizeof(invalid4[0]); ++index) {
		SOCKADDR_STORAGE output;
		memset(&output, 0xa5, sizeof(output));
		int size = sizeof(output);
		TEST_CHECK_EQ(SOCKET_ERROR, WSAStringToAddressW(invalid4[index], AF_INET, NULL, (SOCKADDR *)&output, &size));
		TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
		TEST_CHECK_EQ(sizeof(output), size);
	}
	WCHAR *invalid6[] = {L"[::1", L"[::1]:65536", L"fe80::1%4294967296", L"fe80::1%-1", L"gg::1"};
	for (unsigned index = 0; index < sizeof(invalid6) / sizeof(invalid6[0]); ++index) {
		SOCKADDR_STORAGE output;
		int size = sizeof(output);
		TEST_CHECK_EQ(SOCKET_ERROR, WSAStringToAddressW(invalid6[index], AF_INET6, NULL, (SOCKADDR *)&output, &size));
		TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
	}
	for (unsigned family = 0; family < 2; ++family) {
		SOCKADDR_STORAGE output, previous;
		memset(&output, 0xa5, sizeof(output));
		previous = output;
		int size = family ? sizeof(SOCKADDR_IN6) - 1 : sizeof(SOCKADDR_IN) - 1;
		TEST_CHECK_EQ(SOCKET_ERROR, WSAStringToAddressW(family ? L"::1" : L"127.0.0.1", family ? AF_INET6 : AF_INET,
														NULL, (SOCKADDR *)&output, &size));
		TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
		TEST_CHECK_EQ(family ? sizeof(SOCKADDR_IN6) : sizeof(SOCKADDR_IN), size);
		TEST_CHECK(memcmp(&output, &previous, sizeof(output)) == 0);
	}
	SOCKADDR_STORAGE output;
	int size = sizeof(output);
	TEST_CHECK_EQ(SOCKET_ERROR, WSAStringToAddressW(L"127.0.0.1", 123, NULL, (SOCKADDR *)&output, &size));
	TEST_CHECK_EQ(WSAEINVAL, WSAGetLastError());
	TEST_CHECK_EQ(sizeof(output), size);
	TEST_CHECK_EQ(0, WSACleanup());
	return EXIT_SUCCESS;
}
