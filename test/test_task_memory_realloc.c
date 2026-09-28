#include "test_assert.h"

#include <objbase.h>

#include <string.h>

int main(void) {
	unsigned char *block = (unsigned char *)CoTaskMemRealloc(NULL, 8);
	TEST_CHECK(block != NULL);
	memset(block, 0x5a, 8);
	block = (unsigned char *)CoTaskMemRealloc(block, 64);
	TEST_CHECK(block != NULL);
	for (int i = 0; i < 8; ++i)
		TEST_CHECK_EQ(0x5a, block[i]);
	block = (unsigned char *)CoTaskMemRealloc(block, 4);
	TEST_CHECK(block != NULL);
	for (int i = 0; i < 4; ++i)
		TEST_CHECK_EQ(0x5a, block[i]);
	TEST_CHECK(CoTaskMemRealloc(block, 0) == NULL);
	return 0;
}
