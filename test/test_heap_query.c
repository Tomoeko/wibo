#include "test_assert.h"

#include <windows.h>

int main(void) {
	HANDLE heap = HeapCreate(0, 0, 0);
	TEST_CHECK(heap != NULL);
	ULONG compatibility = 2;
	TEST_CHECK(HeapSetInformation(heap, HeapCompatibilityInformation, &compatibility, sizeof(compatibility)));
	compatibility = 99;
	SIZE_T length = 0;
	TEST_CHECK(
		HeapQueryInformation(heap, HeapCompatibilityInformation, &compatibility, sizeof(compatibility), &length));
	TEST_CHECK_EQ(2, compatibility);
	TEST_CHECK_EQ(sizeof(compatibility), length);
	TEST_CHECK(HeapDestroy(heap));
	return 0;
}
