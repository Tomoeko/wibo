#include <string.h>
#include <windows.h>

#include "test_assert.h"

int main(void) {
	HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
	TEST_CHECK_MSG(kernel32 != NULL, "GetModuleHandleA(kernel32.dll) failed: %lu", (unsigned long)GetLastError());

	SetLastError(0xdeadbeef);
	FARPROC present = GetProcAddress(kernel32, "GetModuleHandleA");
	TEST_CHECK_MSG(present != NULL, "GetProcAddress(GetModuleHandleA) failed: %lu", (unsigned long)GetLastError());

	SetLastError(0xdeadbeef);
	FARPROC missing = GetProcAddress(kernel32, "IsTNT");
	TEST_CHECK(missing == NULL);
	TEST_CHECK_EQ(ERROR_PROC_NOT_FOUND, GetLastError());

#ifdef _WIN64
	char *highName = NULL;
	for (ULONG_PTR i = 1; i <= 16 && !highName; ++i)
		highName = VirtualAlloc((void *)(i << 32), 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(highName != NULL);
	TEST_CHECK(((ULONG_PTR)highName >> 32) != 0);
	TEST_CHECK_EQ(0, (DWORD)(ULONG_PTR)highName);
	strcpy(highName, "GetModuleHandleA");
	TEST_CHECK(GetProcAddress(kernel32, highName) == present);
	FARPROC timeZone = GetProcAddress(kernel32, "GetTimeZoneInformation");
	TEST_CHECK(timeZone != NULL);
	strcpy(highName, "GetTimeZoneInformation");
	TEST_CHECK(GetProcAddress(kernel32, highName) == timeZone);
	strcpy(highName, "IsTNT");
	TEST_CHECK(GetProcAddress(kernel32, highName) == NULL);
	TEST_CHECK_EQ(ERROR_PROC_NOT_FOUND, GetLastError());
	TEST_CHECK(VirtualFree(highName, 0, MEM_RELEASE));
#endif
	return 0;
}
