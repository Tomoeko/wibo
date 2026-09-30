#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

int main(void) {
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	TEST_CHECK(system.dwNumberOfProcessors > 0);
	TEST_CHECK(system.dwActiveProcessorMask != 0);

	SIZE_T largePage = GetLargePageMinimum();
	TEST_CHECK(largePage == 0 || largePage >= 65536);

	PROCESSOR_NUMBER processor = {0, 0, 0};
	USHORT node = 0x1234;
	SetLastError(0x1234);
	if (GetNumaProcessorNodeEx(&processor, &node)) {
		TEST_CHECK_EQ(0, node);
		processor.Group = 0xffff;
		node = 0x1234;
		TEST_CHECK(GetNumaProcessorNodeEx(&processor, &node));
		TEST_CHECK_EQ(0xffff, node);
	} else {
		TEST_CHECK_EQ(ERROR_CALL_NOT_IMPLEMENTED, GetLastError());
		TEST_CHECK_EQ(0x1234, node);
	}

	GROUP_AFFINITY affinity = {0};
	GROUP_AFFINITY previous = {0};
	TEST_CHECK(GetThreadGroupAffinity(GetCurrentThread(), &affinity));
	TEST_CHECK_EQ(0, affinity.Group);
	TEST_CHECK(affinity.Mask != 0);
	TEST_CHECK(SetThreadGroupAffinity(GetCurrentThread(), &affinity, &previous));
	TEST_CHECK_EQ(affinity.Mask, previous.Mask);
	TEST_CHECK_EQ(affinity.Group, previous.Group);
	GROUP_AFFINITY empty = {0};
	TEST_CHECK(!SetThreadGroupAffinity(GetCurrentThread(), &empty, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());

	BYTE *memory =
		(BYTE *)VirtualAllocExNuma(GetCurrentProcess(), NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, 0);
	TEST_CHECK(memory != NULL);
	memory[0] = 0x5a;
	TEST_CHECK_EQ(0x5a, memory[0]);
	TEST_CHECK(VirtualFree(memory, 0, MEM_RELEASE));

	HANDLE process = OpenProcess(PROCESS_VM_OPERATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(process != NULL);
	memory = (BYTE *)VirtualAllocExNuma(process, NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, 0xffff);
	TEST_CHECK(memory != NULL);
	TEST_CHECK(VirtualFree(memory, 0, MEM_RELEASE));
	TEST_CHECK(CloseHandle(process));

	ULONGLONG cycles = 0x123456789abcdef0ULL;
	SetLastError(0x1234);
	if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
		TEST_CHECK_EQ(ERROR_CALL_NOT_IMPLEMENTED, GetLastError());
		TEST_CHECK_U64_EQ(0x123456789abcdef0ULL, cycles);
	}
	return 0;
}
