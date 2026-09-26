#include "test_assert.h"
#include <winsock2.h>

int main(void) {
	fd_set set;
#ifdef _WIN64
	const SOCKET first = 0x1234567887654321ULL;
	const SOCKET second = 0xabcdef0087654321ULL;
#else
	const SOCKET first = 0x87654321U, second = 0x12345678U;
#endif
	WSASetLastError(0x71);
	FD_ZERO(&set);
	TEST_CHECK_EQ(0, FD_ISSET(first, &set));
	FD_SET(first, &set);
	TEST_CHECK(FD_ISSET(first, &set));
	TEST_CHECK_EQ(0, FD_ISSET(second, &set));
	FD_SET(second, &set);
	TEST_CHECK(FD_ISSET(second, &set));
	FD_CLR(first, &set);
	TEST_CHECK_EQ(0, FD_ISSET(first, &set));
	TEST_CHECK(FD_ISSET(second, &set));
	TEST_CHECK_EQ(0x71, WSAGetLastError());
	HMODULE module = LoadLibraryA("ws2_32.dll");
	TEST_CHECK(module != NULL);
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		typedef int(WINAPI * Member)(SOCKET, fd_set *);
		Member member = (Member)(void *)GetProcAddress(module, MAKEINTRESOURCEA(151));
		TEST_CHECK(member != NULL);
		TEST_CHECK(member(second, &set));
		TEST_CHECK_EQ(0, member(first, &set));
	}
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
