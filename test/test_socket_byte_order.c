#include "test_assert.h"
#include <winsock2.h>

int main(void) {
	SetLastError(0x2345);
	TEST_CHECK_U64_EQ(0x78563412, ntohl(0x12345678));
	TEST_CHECK_U64_EQ(0x01000000, ntohl(1));
	TEST_CHECK_U64_EQ(0, ntohl(0));
	TEST_CHECK_U64_EQ(0xffffffff, ntohl(0xffffffff));
	TEST_CHECK_EQ(0x2345, GetLastError());
	return 0;
}
