#include "test_assert.h"

#include <stdint.h>
#include <windows.h>

#ifdef _WIN64
static int hasStackGuard(const void *stackAddress) {
	MEMORY_BASIC_INFORMATION info;
	if (!VirtualQuery(stackAddress, &info, sizeof(info)))
		return 0;
	uintptr_t cursor = (uintptr_t)info.AllocationBase;
	const uintptr_t end = (uintptr_t)stackAddress;
	while (cursor < end) {
		if (!VirtualQuery((const void *)cursor, &info, sizeof(info)) || !info.RegionSize)
			return 0;
		if (info.Protect & PAGE_GUARD)
			return 1;
		const uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
		if (next <= cursor)
			return 0;
		cursor = next;
	}
	return 0;
}
#endif

int main(void) {
	ULONG initial = 0;
	TEST_CHECK(SetThreadStackGuarantee(&initial));

	ULONG requested = 64 * 1024;
	TEST_CHECK(SetThreadStackGuarantee(&requested));
	TEST_CHECK_EQ(initial, requested);

	ULONG current = 0;
	TEST_CHECK(SetThreadStackGuarantee(&current));
	TEST_CHECK(current >= 64 * 1024 && current >= initial);

	ULONG smaller = 4096;
	TEST_CHECK(SetThreadStackGuarantee(&smaller));
	TEST_CHECK_EQ(current, smaller);
	smaller = 0;
	TEST_CHECK(SetThreadStackGuarantee(&smaller));
	TEST_CHECK_EQ(current, smaller);

#ifdef _WIN64
	TEST_CHECK(hasStackGuard(&current));
#endif
	return 0;
}
