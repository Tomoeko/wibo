#include <windows.h>

#include "test_assert.h"

typedef BOOL(WINAPI *file_api_mode_fn)(void);

int main(void) {
	HMODULE module = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC symbol = GetProcAddress(module, "AreFileApisANSI");
	file_api_mode_fn query = NULL;
	TEST_CHECK(sizeof(query) == sizeof(symbol));
	memcpy(&query, &symbol, sizeof(query));
	TEST_CHECK(query != NULL);
	for (unsigned index = 0; index < 8; ++index) {
		SetLastError(0x4321 + index);
		BOOL result = query();
		DWORD error = GetLastError();
		TEST_CHECK_EQ(TRUE, result != FALSE);
		TEST_CHECK_EQ(0x4321 + index, error);
		printf("call=%u ansi=%d error=%lu\n", index, result != FALSE, (unsigned long)error);
	}
	return 0;
}
