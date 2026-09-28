#include "test_assert.h"

#include <stdint.h>
#include <windows.h>

int main(void) {
	uint32_t source = 0x6a53b714, copied = 0;
	SIZE_T transferred = 0;
	TEST_CHECK(ReadProcessMemory(GetCurrentProcess(), &source, &copied, sizeof(source), &transferred));
	TEST_CHECK_EQ(source, copied);
	TEST_CHECK_EQ(sizeof(source), transferred);

	HANDLE limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(limited != NULL);
	transferred = 9;
	TEST_CHECK(!ReadProcessMemory(limited, &source, &copied, sizeof(source), &transferred));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(0, transferred);
	TEST_CHECK(CloseHandle(limited));
	return 0;
}
