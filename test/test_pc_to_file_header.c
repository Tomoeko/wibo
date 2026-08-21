#include <windows.h>

#include "test_assert.h"

__declspec(dllimport) PVOID WINAPI RtlPcToFileHeader(PVOID PcValue, PVOID *BaseOfImage);

int main(void) {
	PVOID expected = (PVOID)GetModuleHandleA(NULL);
	PVOID base = NULL;
	TEST_CHECK(RtlPcToFileHeader((PVOID)&main, &base) == expected);
	TEST_CHECK(base == expected);

	base = expected;
	TEST_CHECK(RtlPcToFileHeader((PVOID)(ULONG_PTR)1, &base) == NULL);
	TEST_CHECK(base == NULL);
	return 0;
}
