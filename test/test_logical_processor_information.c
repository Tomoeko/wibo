#include <windows.h>

#include "test_assert.h"

int main(void) {
	DWORD active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	TEST_CHECK(active > 0);
	DWORD firstGroup = GetActiveProcessorCount(0);
	TEST_CHECK(firstGroup > 0 && firstGroup <= active);
	SetLastError(ERROR_SUCCESS);
	TEST_CHECK_EQ(0, GetActiveProcessorCount(0xfffe));
	TEST_CHECK(GetLastError() != ERROR_SUCCESS);
	const LOGICAL_PROCESSOR_RELATIONSHIP relationships[] = {RelationProcessorCore, RelationProcessorPackage, RelationGroup};
	for (unsigned int i = 0; i < 3; ++i) {
		DWORD length = 0;
		TEST_CHECK(!GetLogicalProcessorInformationEx(relationships[i], NULL, &length));
		TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
		BYTE *data = HeapAlloc(GetProcessHeap(), 0, length);
		TEST_CHECK(data != NULL);
		TEST_CHECK(GetLogicalProcessorInformationEx(relationships[i], (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)data, &length));
		DWORD offset = 0;
		while (offset < length) {
			PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX entry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(data + offset);
			TEST_CHECK_EQ(relationships[i], entry->Relationship);
			TEST_CHECK(entry->Size > 0 && entry->Size <= length - offset);
			if (relationships[i] == RelationGroup) {
				TEST_CHECK(entry->Group.ActiveGroupCount > 0);
				DWORD total = 0;
				for (WORD group = 0; group < entry->Group.ActiveGroupCount; ++group)
					total += GetActiveProcessorCount(group);
				TEST_CHECK_EQ(active, total);
				TEST_CHECK_EQ(firstGroup, entry->Group.GroupInfo[0].ActiveProcessorCount);
				TEST_CHECK(entry->Group.GroupInfo[0].ActiveProcessorMask != 0);
			} else {
				TEST_CHECK(entry->Processor.GroupCount > 0);
				TEST_CHECK(entry->Processor.GroupMask[0].Mask != 0);
			}
			offset += entry->Size;
		}
		TEST_CHECK_EQ(length, offset);
		TEST_CHECK(HeapFree(GetProcessHeap(), 0, data));
	}
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
