#include "test_assert.h"
#include <winsock2.h>
#include <ws2tcpip.h>

int main(void) {
	WSADATA startup;
	const WORD requested[] = {MAKEWORD(1, 0), MAKEWORD(1, 9), MAKEWORD(2, 0), MAKEWORD(2, 1), MAKEWORD(3, 0)};
	const WORD expected[] = {MAKEWORD(1, 0), MAKEWORD(1, 1), MAKEWORD(2, 0), MAKEWORD(2, 1), MAKEWORD(2, 2)};
	for (unsigned index = 0; index < sizeof(requested) / sizeof(requested[0]); ++index) {
		TEST_CHECK_EQ(0, WSAStartup(requested[index], &startup));
		TEST_CHECK_EQ(expected[index], startup.wVersion);
		TEST_CHECK_EQ(0, WSACleanup());
	}
	TEST_CHECK_EQ(WSAVERNOTSUPPORTED, WSAStartup(0, &startup));
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &startup));
	TEST_CHECK_EQ(MAKEWORD(2, 2), startup.wVersion);
	TEST_CHECK_EQ(MAKEWORD(2, 2), startup.wHighVersion);
	TEST_CHECK(startup.szDescription[0] != 0);
	ADDRINFOA hints, *result = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
	TEST_CHECK_EQ(0, getaddrinfo("127.0.0.1", "53", &hints, &result));
	TEST_CHECK(result != NULL);
	for (ADDRINFOA *entry = result; entry; entry = entry->ai_next) {
		TEST_CHECK_EQ(AF_INET, entry->ai_family);
		TEST_CHECK_EQ(SOCK_STREAM, entry->ai_socktype);
		TEST_CHECK_EQ(IPPROTO_TCP, entry->ai_protocol);
		TEST_CHECK_EQ(sizeof(SOCKADDR_IN), entry->ai_addrlen);
		TEST_CHECK_EQ(AF_INET, entry->ai_addr->sa_family);
		const BYTE *bytes = (const BYTE *)entry->ai_addr;
		const BYTE address[] = {127, 0, 0, 1};
		TEST_CHECK_EQ(0, bytes[2]);
		TEST_CHECK_EQ(53, bytes[3]);
		TEST_CHECK(memcmp(address, bytes + 4, sizeof(address)) == 0);
	}
	freeaddrinfo(result);
	hints.ai_family = AF_INET6;
	result = NULL;
	TEST_CHECK_EQ(0, getaddrinfo("::1", "53", &hints, &result));
	TEST_CHECK(result != NULL);
	TEST_CHECK_EQ(AF_INET6, result->ai_family);
	TEST_CHECK_EQ(sizeof(SOCKADDR_IN6), result->ai_addrlen);
	TEST_CHECK_EQ(AF_INET6, result->ai_addr->sa_family);
	const BYTE *bytes = (const BYTE *)result->ai_addr;
	for (unsigned index = 8; index < 23; ++index)
		TEST_CHECK_EQ(0, bytes[index]);
	TEST_CHECK_EQ(1, bytes[23]);
	freeaddrinfo(result);
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
	TEST_CHECK_EQ(0, getaddrinfo(NULL, "53", &hints, &result));
	bytes = (const BYTE *)result->ai_addr;
	for (unsigned index = 4; index < 8; ++index)
		TEST_CHECK_EQ(0, bytes[index]);
	freeaddrinfo(result);
	hints.ai_flags = AI_CANONNAME;
	TEST_CHECK_EQ(0, getaddrinfo("localhost", "53", &hints, &result));
	TEST_CHECK(result != NULL);
	if (result->ai_canonname)
		TEST_CHECK(result->ai_canonname[0] != 0);
	freeaddrinfo(result);
	hints.ai_flags = AI_NUMERICHOST;
	result = (ADDRINFOA *)(uintptr_t)1;
	TEST_CHECK_EQ(WSAHOST_NOT_FOUND, getaddrinfo("not-a-number", "53", &hints, &result));
	TEST_CHECK(result == NULL);
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		hints.ai_addrlen = 1;
		TEST_CHECK_EQ(WSANO_RECOVERY, getaddrinfo("127.0.0.1", "53", &hints, &result));
		TEST_CHECK(result == NULL);
	}
	freeaddrinfo(NULL);
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
