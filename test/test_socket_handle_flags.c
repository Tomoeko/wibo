#include <winsock2.h>

#include "test_assert.h"
#include <windows.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	for (unsigned noInherit = 0; noInherit < 2; ++noInherit) {
		SOCKET s = WSASocketA(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0,
							  WSA_FLAG_OVERLAPPED | (noInherit ? WSA_FLAG_NO_HANDLE_INHERIT : 0));
		TEST_CHECK(s != INVALID_SOCKET);
		DWORD flags = 0x71;
		SetLastError(0x731);
		TEST_CHECK(GetHandleInformation((HANDLE)s, &flags));
		TEST_CHECK_EQ(noInherit ? 0 : HANDLE_FLAG_INHERIT, flags);
		TEST_CHECK_EQ(0x731, GetLastError());
		TEST_CHECK(SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE,
										HANDLE_FLAG_PROTECT_FROM_CLOSE));
		TEST_CHECK(GetHandleInformation((HANDLE)s, &flags));
		TEST_CHECK_EQ(HANDLE_FLAG_PROTECT_FROM_CLOSE, flags);
		TEST_CHECK(SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
		TEST_CHECK(GetHandleInformation((HANDLE)s, &flags));
		TEST_CHECK_EQ(HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE, flags);
		TEST_CHECK(SetHandleInformation((HANDLE)s, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0));
		TEST_CHECK_EQ(0, closesocket(s));
		flags = 0x71;
		TEST_CHECK(!GetHandleInformation((HANDLE)s, &flags));
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
		TEST_CHECK_EQ(0x71, flags);
		TEST_CHECK(!SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0));
		TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	}
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
