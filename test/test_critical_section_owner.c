#include <windows.h>

#include "test_assert.h"

// A valid initialized section is observed only while its owner is stable.
// https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/displaying-a-critical-section
// Resolve the 32-bit logical return used by the exported ntdll query.
typedef LONG(WINAPI *OwnerQuery)(PCRITICAL_SECTION);

static OwnerQuery query_owner;
static const DWORD kLastError = 0x13572468;

typedef struct {
	CRITICAL_SECTION *section;
	HANDLE inspected;
	HANDLE may_enter;
	HANDLE acquired;
	HANDLE may_leave;
} WorkerState;

static void check_owner(CRITICAL_SECTION *section, int expected) {
	CRITICAL_SECTION before = *section;
	SetLastError(kLastError);
	LONG result = query_owner(section);
	TEST_CHECK_EQ(expected, result != 0);
	TEST_CHECK_EQ(kLastError, GetLastError());
	TEST_CHECK_EQ(0, memcmp(&before, section, sizeof(before)));
}

static DWORD WINAPI worker(LPVOID parameter) {
	WorkerState *state = parameter;
	check_owner(state->section, 0);
	TEST_CHECK(SetEvent(state->inspected));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state->may_enter, 5000));
	EnterCriticalSection(state->section);
	check_owner(state->section, 1);
	EnterCriticalSection(state->section);
	check_owner(state->section, 1);
	LeaveCriticalSection(state->section);
	check_owner(state->section, 1);
	TEST_CHECK(SetEvent(state->acquired));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state->may_leave, 5000));
	LeaveCriticalSection(state->section);
	check_owner(state->section, 0);
	return 0;
}

int main(void) {
	HMODULE module = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(module != NULL);
	query_owner = (OwnerQuery)(void *)GetProcAddress(module, "RtlIsCriticalSectionLockedByThread");
	TEST_CHECK(query_owner != NULL);
	CRITICAL_SECTION section;
	InitializeCriticalSection(&section);
	check_owner(&section, 0);
	EnterCriticalSection(&section);
	check_owner(&section, 1);
	EnterCriticalSection(&section);
	check_owner(&section, 1);
	LeaveCriticalSection(&section);
	check_owner(&section, 1);

	WorkerState state = {&section, CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL),
						 CreateEventA(NULL, FALSE, FALSE, NULL), CreateEventA(NULL, FALSE, FALSE, NULL)};
	TEST_CHECK(state.inspected && state.may_enter && state.acquired && state.may_leave);
	HANDLE thread = CreateThread(NULL, 0, worker, &state, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.inspected, 5000));
	LeaveCriticalSection(&section);
	check_owner(&section, 0);
	TEST_CHECK(SetEvent(state.may_enter));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.acquired, 5000));
	check_owner(&section, 0);
	TEST_CHECK(SetEvent(state.may_leave));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	DWORD code = 99;
	TEST_CHECK(GetExitCodeThread(thread, &code));
	TEST_CHECK_EQ(0, code);
	check_owner(&section, 0);
	TEST_CHECK(TryEnterCriticalSection(&section));
	check_owner(&section, 1);
	LeaveCriticalSection(&section);
	check_owner(&section, 0);
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(state.inspected));
	TEST_CHECK(CloseHandle(state.may_enter));
	TEST_CHECK(CloseHandle(state.acquired));
	TEST_CHECK(CloseHandle(state.may_leave));
	DeleteCriticalSection(&section);
	puts("critical section ownership tests passed");
	return 0;
}
