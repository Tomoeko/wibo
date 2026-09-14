#include <windows.h>

#include <stdint.h>

#include "test_assert.h"

// https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery
// Query live, owned stack objects only. A worker remains alive while the main
// thread queries it; no guard pages, retired pointers, or memory faults are used.
struct WorkerState {
	HANDLE ready;
	HANDLE release;
	void *volatile address;
	PVOID allocation_base;
};

static PVOID check_stack_byte(const char *label, const void *address) {
	MEMORY_BASIC_INFORMATION info;
	memset(&info, 0, sizeof(info));
	const DWORD sentinel = 0x13572468;
	SetLastError(sentinel);
	TEST_CHECK_EQ(sizeof(info), VirtualQuery(address, &info, sizeof(info)));
	TEST_CHECK_MSG(info.State == MEM_COMMIT, "%s: state 0x%lx", label, info.State);
	TEST_CHECK_EQ(MEM_PRIVATE, info.Type);
	TEST_CHECK_EQ(PAGE_READWRITE, info.Protect);
	TEST_CHECK(info.AllocationBase != NULL);
	TEST_CHECK((uintptr_t)info.AllocationBase <= (uintptr_t)info.BaseAddress);
	TEST_CHECK((uintptr_t)info.BaseAddress <= (uintptr_t)address);
	TEST_CHECK((uintptr_t)address - (uintptr_t)info.BaseAddress < info.RegionSize);
	TEST_CHECK_EQ(FALSE, IsBadCodePtr((FARPROC)(void *)address));
	TEST_CHECK_EQ(sentinel, GetLastError());
	return info.AllocationBase;
}

static DWORD WINAPI worker(void *argument) {
	struct WorkerState *state = argument;
	unsigned char owned[64];
	memset(owned, 0x5a, sizeof(owned));
	state->allocation_base = check_stack_byte("worker first byte", owned);
	TEST_CHECK(state->allocation_base == check_stack_byte("worker last byte", owned + sizeof(owned) - 1));
	state->address = owned;
	TEST_CHECK(SetEvent(state->ready));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state->release, 10000));
	for (unsigned i = 0; i < sizeof(owned); ++i)
		TEST_CHECK_EQ(0x5a, owned[i]);
	return 0;
}

int main(void) {
	unsigned char owned[64];
	memset(owned, 0xa5, sizeof(owned));
	PVOID main_base = check_stack_byte("main first byte", owned);
	TEST_CHECK(main_base == check_stack_byte("main last byte", owned + sizeof(owned) - 1));
	struct WorkerState state = {0};
	state.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
	state.release = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(state.ready != NULL && state.release != NULL);
	HANDLE thread = CreateThread(NULL, 0, worker, &state, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.ready, 10000));
	TEST_CHECK(state.address != NULL);
	TEST_CHECK(state.allocation_base != main_base);
	TEST_CHECK(state.allocation_base == check_stack_byte("held worker from main", state.address));
	TEST_CHECK(SetEvent(state.release));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 10000));
	DWORD exit_code = STILL_ACTIVE;
	TEST_CHECK(GetExitCodeThread(thread, &exit_code));
	TEST_CHECK_EQ(0, exit_code);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(state.ready));
	TEST_CHECK(CloseHandle(state.release));
	for (unsigned i = 0; i < sizeof(owned); ++i)
		TEST_CHECK_EQ(0xa5, owned[i]);
	puts("stack query tests passed: main, worker, held cross-thread metadata; no faults");
	return 0;
}
