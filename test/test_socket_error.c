#include "test_assert.h"
#include <winsock2.h>

static DWORD WINAPI worker(void *unused) {
	(void)unused;
	WSASetLastError(WSAEADDRINUSE);
	TEST_CHECK_EQ(WSAEADDRINUSE, WSAGetLastError());
	TEST_CHECK_EQ(WSAEADDRINUSE, GetLastError());
	return 0;
}
int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	WSASetLastError(WSAECONNREFUSED);
	TEST_CHECK_EQ(WSAECONNREFUSED, WSAGetLastError());
	TEST_CHECK_EQ(WSAECONNREFUSED, GetLastError());
	SetLastError(WSAEFAULT);
	TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
	HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 10000));
	TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
	TEST_CHECK(CloseHandle(thread));
	void(WINAPI * setter)(int) = (void *)GetProcAddress(GetModuleHandleA("ws2_32.dll"), (LPCSTR)112);
	TEST_CHECK(setter != NULL);
	setter(0);
	TEST_CHECK_EQ(0, WSAGetLastError());
	TEST_CHECK_EQ(0, GetLastError());
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
