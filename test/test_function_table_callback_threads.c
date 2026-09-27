#include "test_assert.h"
#include <windows.h>

typedef BOOLEAN(WINAPI *install_fn)(DWORD64, DWORD64, DWORD, PGET_RUNTIME_FUNCTION_CALLBACK, PVOID, PCWSTR);
typedef BOOLEAN(WINAPI *delete_fn)(PRUNTIME_FUNCTION);
typedef PRUNTIME_FUNCTION(WINAPI *lookup_fn)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);

static install_fn install;
static delete_fn remove_table;
static lookup_fn lookup;

struct callback_state {
	DWORD64 base;
	RUNTIME_FUNCTION entry;
	HANDLE entered;
	HANDLE secondEntered;
	HANDLE release;
	volatile LONG calls;
	volatile LONG active;
	volatile LONG maximumActive;
};

struct worker_state {
	struct callback_state *callback;
	HANDLE started;
	DWORD64 identifier;
	DWORD64 imageBase;
	PRUNTIME_FUNCTION result;
	BOOLEAN removed;
};

static PRUNTIME_FUNCTION CALLBACK resolve_entry(DWORD64 pc, PVOID context) {
	struct callback_state *state = context;
	LONG active = InterlockedIncrement(&state->active);
	LONG maximum = InterlockedCompareExchange(&state->maximumActive, 0, 0);
	while (active > maximum) {
		LONG previous = InterlockedCompareExchange(&state->maximumActive, active, maximum);
		if (previous == maximum)
			break;
		maximum = previous;
	}
	LONG calls = InterlockedIncrement(&state->calls);
	TEST_CHECK(SetEvent(calls == 1 ? state->entered : state->secondEntered));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state->release, 10000));
	InterlockedDecrement(&state->active);
	TEST_CHECK_U64_EQ(state->base + state->entry.BeginAddress, pc);
	return &state->entry;
}

static DWORD WINAPI lookup_worker(PVOID argument) {
	struct worker_state *state = argument;
	TEST_CHECK(SetEvent(state->started));
	state->result = lookup(state->callback->base + state->callback->entry.BeginAddress, &state->imageBase, NULL);
	return 0;
}

static DWORD WINAPI delete_worker(PVOID argument) {
	struct worker_state *state = argument;
	TEST_CHECK(SetEvent(state->started));
	state->removed = remove_table((PRUNTIME_FUNCTION)(ULONG_PTR)state->identifier);
	return 0;
}

static struct callback_state create_state(DWORD64 base) {
	struct callback_state state = {base, {16, 32, 256}, NULL, NULL, NULL, 0, 0, 0};
	state.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.secondEntered = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(state.entered && state.secondEntered && state.release);
	return state;
}

static void close_state(struct callback_state *state) {
	TEST_CHECK(CloseHandle(state->entered));
	TEST_CHECK(CloseHandle(state->secondEntered));
	TEST_CHECK(CloseHandle(state->release));
}

static struct worker_state create_worker(struct callback_state *callback) {
	struct worker_state state = {callback, NULL, callback->base | 3, 0, NULL, FALSE};
	state.started = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(state.started != NULL);
	return state;
}

static void check_delete_serialization(DWORD64 base) {
	struct callback_state state = create_state(base);
	struct worker_state first = create_worker(&state);
	struct worker_state deletion = create_worker(&state);
	TEST_CHECK(install(base | 3, base, 128, resolve_entry, &state, NULL));
	HANDLE firstThread = CreateThread(NULL, 0, lookup_worker, &first, 0, NULL);
	TEST_CHECK(firstThread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.entered, 5000));
	HANDLE deleteThread = CreateThread(NULL, 0, delete_worker, &deletion, 0, NULL);
	TEST_CHECK(deleteThread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(deletion.started, 5000));
	DWORD beforeRelease = WaitForSingleObject(deleteThread, 250);
	TEST_CHECK(SetEvent(state.release));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(firstThread, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(deleteThread, 5000));
	TEST_CHECK(first.result == &state.entry);
	TEST_CHECK_U64_EQ(base, first.imageBase);
	TEST_CHECK(deletion.removed);
	TEST_CHECK(CloseHandle(firstThread));
	TEST_CHECK(CloseHandle(deleteThread));
	TEST_CHECK(CloseHandle(first.started));
	TEST_CHECK(CloseHandle(deletion.started));
	close_state(&state);
	printf("Deletion wait before callback release: %lu\n", (unsigned long)beforeRelease);
	TEST_CHECK_EQ(WAIT_TIMEOUT, beforeRelease);
}

static void check_lookup_serialization(DWORD64 base) {
	struct callback_state state = create_state(base);
	struct worker_state first = create_worker(&state);
	struct worker_state second = create_worker(&state);
	TEST_CHECK(install(base | 3, base, 128, resolve_entry, &state, NULL));
	HANDLE firstThread = CreateThread(NULL, 0, lookup_worker, &first, 0, NULL);
	TEST_CHECK(firstThread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.entered, 5000));
	HANDLE secondThread = CreateThread(NULL, 0, lookup_worker, &second, 0, NULL);
	TEST_CHECK(secondThread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(second.started, 5000));
	DWORD beforeRelease = WaitForSingleObject(state.secondEntered, 250);
	TEST_CHECK(SetEvent(state.release));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(firstThread, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(secondThread, 5000));
	TEST_CHECK(first.result == &state.entry);
	TEST_CHECK(second.result == &state.entry);
	TEST_CHECK_U64_EQ(base, first.imageBase);
	TEST_CHECK_U64_EQ(base, second.imageBase);
	TEST_CHECK(remove_table((PRUNTIME_FUNCTION)(ULONG_PTR)(base | 3)));
	TEST_CHECK(CloseHandle(firstThread));
	TEST_CHECK(CloseHandle(secondThread));
	TEST_CHECK(CloseHandle(first.started));
	TEST_CHECK(CloseHandle(second.started));
	close_state(&state);
	printf("Concurrent callback count: %ld\n", (long)state.maximumActive);
	TEST_CHECK_EQ(WAIT_TIMEOUT, beforeRelease);
	TEST_CHECK_EQ(1, state.maximumActive);
	TEST_CHECK_EQ(2, state.calls);
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"kernel32.dll");
	HMODULE native = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(module && native);
	install = (install_fn)(ULONG_PTR)GetProcAddress(module, "RtlInstallFunctionTableCallback");
	remove_table = (delete_fn)(ULONG_PTR)GetProcAddress(module, "RtlDeleteFunctionTable");
	lookup = (lookup_fn)(ULONG_PTR)GetProcAddress(native, "RtlLookupFunctionEntry");
	TEST_CHECK(install && remove_table && lookup);
	BYTE *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	TEST_CHECK(code != NULL);
	code[256] = 1;
	check_delete_serialization((DWORD64)(ULONG_PTR)code);
	check_lookup_serialization((DWORD64)(ULONG_PTR)code);
	TEST_CHECK(VirtualFree(code, 0, MEM_RELEASE));
	return 0;
}
