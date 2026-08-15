#include "test_assert.h"
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

static void test_startup_gethostname_cleanup(void) {
	WSADATA data;
	memset(&data, 0, sizeof(data));

	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(1, 1), &data));
	TEST_CHECK_EQ(1, data.wVersion & 0xff);
	TEST_CHECK_EQ(1, (data.wVersion >> 8) & 0xff);

	char hostname[256] = {0};
	TEST_CHECK_EQ(0, gethostname(hostname, sizeof(hostname)));
	TEST_CHECK(hostname[0] != '\0');

	TEST_CHECK_EQ(0, WSACleanup());
}

static void test_ordinal_exports_cross_the_abi_boundary(void) {
	typedef int(WINAPI * startup_fn)(WORD, LPWSADATA);
	typedef int(WINAPI * cleanup_fn)(void);
	HMODULE module = LoadLibraryA("ws2_32.dll");
	TEST_CHECK(module != NULL);
	startup_fn startup = (startup_fn)(uintptr_t)GetProcAddress(module, (LPCSTR)(uintptr_t)115);
	cleanup_fn cleanup = (cleanup_fn)(uintptr_t)GetProcAddress(module, (LPCSTR)(uintptr_t)116);
	TEST_CHECK(startup != NULL);
	TEST_CHECK(cleanup != NULL);

	WSADATA data;
	memset(&data, 0, sizeof(data));
	TEST_CHECK_EQ(0, startup(MAKEWORD(1, 1), &data));
	TEST_CHECK_EQ(0, cleanup());
	TEST_CHECK(FreeLibrary(module));
}

int main(void) {
	test_startup_gethostname_cleanup();
	test_ordinal_exports_cross_the_abi_boundary();
	return EXIT_SUCCESS;
}
