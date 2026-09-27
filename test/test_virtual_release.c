#include "test_assert.h"
#include <stdint.h>
#include <windows.h>

int main(void) {
	SYSTEM_INFO system;
	GetSystemInfo(&system);
	const SIZE_T size = system.dwAllocationGranularity;
	uint8_t *memory = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(memory != NULL);
	for (unsigned attempt = 0; attempt < 4; ++attempt) {
		memory[0] = 0x51;
		memory[size - 1] = 0x62;
		TEST_CHECK(VirtualFree(memory, 0, MEM_RELEASE));
		MEMORY_BASIC_INFORMATION state;
		TEST_CHECK_EQ(sizeof(state), VirtualQuery(memory, &state, sizeof(state)));
		TEST_CHECK_EQ(MEM_FREE, state.State);
		TEST_CHECK(state.AllocationBase == NULL);
		uint8_t *replacement = VirtualAlloc(memory, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		TEST_CHECK_MSG(replacement == memory, "Released address cannot be reused: %lu", GetLastError());
		TEST_CHECK_EQ(0, replacement[0]);
		TEST_CHECK_EQ(0, replacement[size - 1]);
	}
	TEST_CHECK(VirtualFree(memory, 0, MEM_RELEASE));
	return 0;
}
