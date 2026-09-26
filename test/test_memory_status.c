#include "test_assert.h"
#include <windows.h>

int main(void) {
	MEMORYSTATUSEX status;
	memset(&status, 0x55, sizeof(status));
	status.dwLength = sizeof(status) - 1;
	MEMORYSTATUSEX previous = status;
	TEST_CHECK(!GlobalMemoryStatusEx(&status));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(memcmp(&previous, &status, sizeof(status)) == 0);
	status.dwLength = sizeof(status);
	TEST_CHECK(GlobalMemoryStatusEx(&status));
	TEST_CHECK_EQ(sizeof(status), status.dwLength);
	TEST_CHECK(status.dwMemoryLoad <= 100);
	TEST_CHECK(status.ullTotalPhys > 0 && status.ullAvailPhys <= status.ullTotalPhys);
	TEST_CHECK(status.ullTotalPageFile > 0 && status.ullAvailPageFile <= status.ullTotalPageFile);
	TEST_CHECK(status.ullTotalVirtual > 0 && status.ullAvailVirtual <= status.ullTotalVirtual);
	TEST_CHECK_EQ(0, status.ullAvailExtendedVirtual);
	const ULONGLONG before = status.ullAvailVirtual;
	const SIZE_T size = 16 * 1024 * 1024;
	void *allocation = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
	TEST_CHECK(allocation != NULL);
	TEST_CHECK(GlobalMemoryStatusEx(&status));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK(status.ullAvailVirtual < before);
	const ULONGLONG reserved = status.ullAvailVirtual;
	TEST_CHECK(VirtualFree(allocation, 0, MEM_RELEASE));
	TEST_CHECK(GlobalMemoryStatusEx(&status));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK(status.ullAvailVirtual > reserved);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(25, status.dwMemoryLoad);
		TEST_CHECK_U64_EQ(16ULL << 30, status.ullTotalPhys);
		const char *faults[] = {"truncated", "bad-load"};
		previous = status;
		for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
			TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_MEMORY_RESPONSE", faults[i]));
			TEST_CHECK(!GlobalMemoryStatusEx(&status));
			TEST_CHECK_EQ(ERROR_INVALID_DATA, GetLastError());
			TEST_CHECK(memcmp(&previous, &status, sizeof(status)) == 0);
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_MEMORY_RESPONSE", NULL));
	}
	return 0;
}
