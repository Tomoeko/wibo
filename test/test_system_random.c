#include "test_assert.h"
#include <windows.h>

typedef BOOLEAN(WINAPI *RandomFunction)(PVOID, ULONG);

int main(void) {
	HMODULE module = LoadLibraryA("advapi32.dll");
	TEST_CHECK(module != NULL);
	RandomFunction random = (RandomFunction)GetProcAddress(module, "SystemFunction036");
	TEST_CHECK(random != NULL);
	TEST_CHECK(random(NULL, 0));
	BYTE buffers[2][8194];
	memset(buffers, 0xcc, sizeof(buffers));
	for (unsigned i = 0; i < 2; ++i) {
		SetLastError(0x731);
		TEST_CHECK(random(buffers[i] + 1, 8192));
		TEST_CHECK_EQ(0x731, GetLastError());
		TEST_CHECK_EQ(0xcc, buffers[i][0]);
		TEST_CHECK_EQ(0xcc, buffers[i][8193]);
	}
	TEST_CHECK(memcmp(buffers[0] + 1, buffers[1] + 1, 8192) != 0);
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
