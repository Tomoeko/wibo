#include "test_assert.h"
#include <stdint.h>
#include <windows.h>

int main(void) {
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	const SIZE_T page = info.dwPageSize;
	uint8_t *memory = VirtualAlloc(NULL, page * 2, MEM_RESERVE, PAGE_NOACCESS);
	TEST_CHECK(memory != NULL);
	TEST_CHECK(VirtualAlloc(memory, page, MEM_COMMIT, PAGE_READWRITE) == memory);
	const uint8_t code[] = {0xB8, 37, 0, 0, 0, 0xC3}; // mov eax, 37; ret
	CopyMemory(memory, code, sizeof(code));
	memory[page - 1] = 0x51;
	const DWORD protections[] = {PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_READ, PAGE_READONLY, PAGE_READWRITE};
	for (unsigned i = 0; i < sizeof(protections) / sizeof(protections[0]); ++i) {
		SetLastError(0x71);
		TEST_CHECK(VirtualAlloc(memory, page, MEM_COMMIT, protections[i]) == memory);
		TEST_CHECK_EQ(0x71, GetLastError());
		MEMORY_BASIC_INFORMATION state;
		TEST_CHECK_EQ(sizeof(state), VirtualQuery(memory, &state, sizeof(state)));
		TEST_CHECK_EQ(protections[i], state.Protect);
		TEST_CHECK_EQ(MEM_COMMIT, state.State);
		TEST_CHECK_EQ(37, memory[1]);
		TEST_CHECK_EQ(0x51, memory[page - 1]);
#ifdef _WIN64
		// Executable probes use the x64 guest ABI.
		if (i < 2) {
			TEST_CHECK(FlushInstructionCache(GetCurrentProcess(), memory, sizeof(code)));
			TEST_CHECK_EQ(37, ((int (*)(void))(uintptr_t)memory)());
			if (i == 0) {
				memory[1] = 63;
				TEST_CHECK(FlushInstructionCache(GetCurrentProcess(), memory, sizeof(code)));
				TEST_CHECK_EQ(63, ((int (*)(void))(uintptr_t)memory)());
				memory[1] = 37;
				TEST_CHECK(FlushInstructionCache(GetCurrentProcess(), NULL, 0));
				TEST_CHECK_EQ(37, ((int (*)(void))(uintptr_t)memory)());
			}
		}
#endif
	}
	TEST_CHECK(VirtualAlloc(memory + page - 1, 2, MEM_COMMIT, PAGE_READONLY) == memory);
	MEMORY_BASIC_INFORMATION state;
	TEST_CHECK_EQ(sizeof(state), VirtualQuery(memory, &state, sizeof(state)));
	TEST_CHECK_EQ(PAGE_READONLY, state.Protect);
	TEST_CHECK_EQ(page * 2, state.RegionSize);
	TEST_CHECK_EQ(0x51, memory[page - 1]);
	for (SIZE_T i = page; i < page * 2; ++i)
		TEST_CHECK_EQ(0, memory[i]);
	TEST_CHECK(VirtualAlloc(memory, page * 2, MEM_COMMIT, PAGE_READWRITE) == memory);
	memory[page] = 0x62;
	TEST_CHECK_EQ(37, memory[1]);
	TEST_CHECK_EQ(0x62, memory[page]);
	TEST_CHECK(VirtualFree(memory, 0, MEM_RELEASE));
	return 0;
}
