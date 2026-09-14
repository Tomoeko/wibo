#include <windows.h>

#include "test_assert.h"

// This API checks read access, not whether bytes form executable code.
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-isbadcodeptr
// Every address below belongs to a live, readable object. No guest fault,
// inaccessible address, guard page, or concurrent protection change is tested.
static volatile unsigned int calls;
static const unsigned char constant_data[] = {0x12, 0x34, 0x56, 0x78};

static INT_PTR WINAPI fixture_function(void) {
	++calls;
	return 0;
}

static void check_readable(const char *label, FARPROC pointer) {
	const DWORD sentinel = 0x13572468;
	SetLastError(sentinel);
	BOOL result = IsBadCodePtr(pointer);
	DWORD error = GetLastError();
	TEST_CHECK_MSG(result == FALSE, "%s returned %d", label, result);
	TEST_CHECK_EQ(sentinel, error);
	TEST_CHECK_EQ(0, calls);
}

int main(void) {
	unsigned char stack_data[] = {0xa1, 0xb2, 0xc3, 0xd4};
	check_readable("function", (FARPROC)fixture_function);
	check_readable("stack data", (FARPROC)(void *)stack_data);
	check_readable("constant data", (FARPROC)(const void *)constant_data);
	unsigned char *allocation = VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(allocation != NULL);
	memset(allocation, 0x5a, 64);
	check_readable("allocated data", (FARPROC)(void *)allocation);
	DWORD previous = 0;
	TEST_CHECK(VirtualProtect(allocation, 64, PAGE_READONLY, &previous));
	TEST_CHECK_EQ(PAGE_READWRITE, previous);
	check_readable("read-only allocation", (FARPROC)(void *)allocation);
	check_readable("last owned read-only byte", (FARPROC)(void *)(allocation + 63));
	TEST_CHECK_EQ(0x5a, allocation[0]);
	TEST_CHECK_EQ(0x5a, allocation[63]);
	TEST_CHECK_EQ(0xa1, stack_data[0]);
	TEST_CHECK_EQ(0xd4, stack_data[3]);
	TEST_CHECK_EQ(0x12, constant_data[0]);
	TEST_CHECK_EQ(0x78, constant_data[3]);
	TEST_CHECK(VirtualFree(allocation, 0, MEM_RELEASE));
	puts("code pointer readability tests passed: six live readable addresses, no calls");
	return 0;
}
