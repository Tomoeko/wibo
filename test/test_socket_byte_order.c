#include "test_assert.h"
#include <winsock2.h>

int main(void) {
	SetLastError(0x2345);
	TEST_CHECK_U64_EQ(0x78563412, ntohl(0x12345678));
	TEST_CHECK_U64_EQ(0x01000000, ntohl(1));
	TEST_CHECK_U64_EQ(0, ntohl(0));
	TEST_CHECK_U64_EQ(0xffffffff, ntohl(0xffffffff));
	TEST_CHECK_U64_EQ(0x78563412, htonl(0x12345678));
	TEST_CHECK_EQ(0x01FE, ntohs(0xFE01));
	TEST_CHECK_EQ(0x01FE, htons(0xFE01));
	HMODULE module = LoadLibraryA("ws2_32.dll");
	typedef u_short(WINAPI * convert_fn)(u_short);
	convert_fn convert = (convert_fn)GetProcAddress(module, (LPCSTR)(uintptr_t)15);
	TEST_CHECK(convert != NULL);
	TEST_CHECK_EQ(0x01FE, convert(0xFE01));
	SetLastError(0x2345);
	TEST_CHECK_EQ(0x01FE, convert(0xFE01));
	TEST_CHECK_EQ(0x2345, GetLastError());
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
