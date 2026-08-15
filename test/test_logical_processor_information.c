#include <windows.h>

#include "test_assert.h"

int main(void) {
	DWORD bytes = 0;
	TEST_CHECK(!GetLogicalProcessorInformation(NULL, &bytes));
	TEST_CHECK(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
	TEST_CHECK(bytes >= sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));

	HANDLE heap = GetProcessHeap();
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION records =
		(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)HeapAlloc(heap, HEAP_ZERO_MEMORY, bytes);
	TEST_CHECK(records != NULL);
	TEST_CHECK(GetLogicalProcessorInformation(records, &bytes));

	DWORD count = bytes / sizeof(*records);
	DWORD cores = 0;
	DWORD packages = 0;
	for (DWORD index = 0; index < count; ++index) {
		if (records[index].Relationship == RelationProcessorCore) {
			TEST_CHECK(records[index].ProcessorMask != 0);
			++cores;
		} else if (records[index].Relationship == RelationProcessorPackage) {
			TEST_CHECK(records[index].ProcessorMask != 0);
			++packages;
		}
	}
	TEST_CHECK(cores > 0);
	TEST_CHECK(packages > 0);
	TEST_CHECK(HeapFree(heap, 0, records));
	return 0;
}
