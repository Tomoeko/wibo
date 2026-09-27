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
	DWORD64 identifier;
	RUNTIME_FUNCTION entry;
	DWORD calls;
	DWORD64 lastPc;
	BOOL returnNull;
	struct callback_state *replacement;
	DWORD callbackError;
};

static PRUNTIME_FUNCTION CALLBACK resolve_entry(DWORD64 pc, PVOID context) {
	struct callback_state *state = context;
	++state->calls;
	state->lastPc = pc;
	if (state->callbackError)
		SetLastError(state->callbackError);
	if (state->replacement) {
		struct callback_state *replacement = state->replacement;
		state->replacement = NULL;
		TEST_CHECK(remove_table((PRUNTIME_FUNCTION)(ULONG_PTR)state->identifier));
		TEST_CHECK(install(replacement->identifier, replacement->base, 128, resolve_entry, replacement, NULL));
		DWORD64 base = 0;
		TEST_CHECK(lookup(replacement->base + replacement->entry.BeginAddress, &base, NULL) == &replacement->entry);
		TEST_CHECK_U64_EQ(replacement->base, base);
	}
	if (state->returnNull || pc < state->base + state->entry.BeginAddress ||
		pc >= state->base + state->entry.EndAddress)
		return NULL;
	return &state->entry;
}

static void expect_entry(struct callback_state *state, DWORD offset, PRUNTIME_FUNCTION expected, DWORD calls) {
	const DWORD64 pc = state->base + offset;
	DWORD64 base = 0x12345678;
	SetLastError(0x71);
	TEST_CHECK(lookup(pc, &base, NULL) == expected);
	TEST_CHECK_EQ(calls, state->calls);
	if (expected) {
		TEST_CHECK_U64_EQ(state->base, base);
		TEST_CHECK_U64_EQ(pc, state->lastPc);
	} else
		TEST_CHECK_U64_EQ(0, base);
	TEST_CHECK_EQ(0x71, GetLastError());
}

static void delete_registration(DWORD64 identifier, BOOL expected) {
	SetLastError(0x71);
	TEST_CHECK_EQ(expected, remove_table((PRUNTIME_FUNCTION)(ULONG_PTR)identifier));
	TEST_CHECK_EQ(0x71, GetLastError());
}

int main(void) {
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	HMODULE native = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(kernel && native);
	install = (install_fn)(ULONG_PTR)GetProcAddress(kernel, "RtlInstallFunctionTableCallback");
	remove_table = (delete_fn)(ULONG_PTR)GetProcAddress(kernel, "RtlDeleteFunctionTable");
	lookup = (lookup_fn)(ULONG_PTR)GetProcAddress(native, "RtlLookupFunctionEntry");
	install_fn native_install = (install_fn)(ULONG_PTR)GetProcAddress(native, "RtlInstallFunctionTableCallback");
	delete_fn native_remove = (delete_fn)(ULONG_PTR)GetProcAddress(native, "RtlDeleteFunctionTable");
	TEST_CHECK(install && remove_table && lookup && native_install && native_remove);
	const DWORD64 moduleBase = (DWORD64)(ULONG_PTR)GetModuleHandleW(NULL);
	DWORD64 imageBase = 0x12345678;
	TEST_CHECK(lookup(moduleBase, &imageBase, NULL) == NULL);
	TEST_CHECK_U64_EQ(moduleBase, imageBase);
	BYTE *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	TEST_CHECK(code != NULL);
	code[256] = 1;
	const DWORD64 base = (DWORD64)(ULONG_PTR)code;
	const DWORD64 identifier = base | 3;
	struct callback_state first = {base, identifier, {16, 32, 256}, 0, 0, FALSE, NULL, 0};
	struct callback_state second = {base, identifier, {16, 32, 256}, 0, 0, FALSE, NULL, 0};
	for (DWORD bits = 0; bits < 3; ++bits) {
		SetLastError(0x71);
		TEST_CHECK(!install(base | bits, base, 128, resolve_entry, &first, NULL));
		TEST_CHECK_EQ(0x71, GetLastError());
	}
	SetLastError(0x71);
	TEST_CHECK(install(identifier, base, 128, resolve_entry, &first, NULL));
	TEST_CHECK_EQ(0x71, GetLastError());
	TEST_CHECK_EQ(0, first.calls);
	imageBase = 0x12345678;
	TEST_CHECK(lookup(base - 1, &imageBase, NULL) == NULL);
	TEST_CHECK_U64_EQ(0, imageBase);
	TEST_CHECK_EQ(0, first.calls);
	expect_entry(&first, 128, NULL, 0);
	expect_entry(&first, 16, &first.entry, 1);
	expect_entry(&first, 31, &first.entry, 2);
	expect_entry(&first, 32, NULL, 3);
	delete_registration(identifier, TRUE);
	delete_registration(identifier, FALSE);
	expect_entry(&first, 16, NULL, 3);

	first.calls = 0;
	first.returnNull = TRUE;
	TEST_CHECK(native_install(identifier, base, 128, resolve_entry, &first, NULL));
	TEST_CHECK(install(identifier, base, 128, resolve_entry, &second, NULL));
	expect_entry(&first, 16, NULL, 1);
	TEST_CHECK_EQ(0, second.calls);
	TEST_CHECK(native_remove((PRUNTIME_FUNCTION)(ULONG_PTR)identifier));
	expect_entry(&second, 16, &second.entry, 1);
	delete_registration(identifier, TRUE);
	delete_registration(identifier, FALSE);

	first.returnNull = FALSE;
	first.callbackError = 0x72;
	TEST_CHECK(install(identifier, base, 128, resolve_entry, &first, NULL));
	SetLastError(0x71);
	TEST_CHECK(lookup(base + 16, &imageBase, NULL) == &first.entry);
	TEST_CHECK_U64_EQ(base, imageBase);
	TEST_CHECK_EQ(0x72, GetLastError());
	delete_registration(identifier, TRUE);
	first.callbackError = 0;

	first.returnNull = FALSE;
	first.calls = 0;
	TEST_CHECK(install(identifier, base, 0, resolve_entry, &first, NULL));
	expect_entry(&first, 16, NULL, 0);
	delete_registration(identifier, TRUE);
	TEST_CHECK(install(identifier, base, 128, NULL, NULL, NULL));
	delete_registration(identifier, TRUE);

	first.replacement = &second;
	second.base = base + 512;
	second.calls = 0;
	TEST_CHECK(install(identifier, base, 128, resolve_entry, &first, NULL));
	expect_entry(&first, 16, &first.entry, 1);
	TEST_CHECK_EQ(1, second.calls);
	expect_entry(&second, 16, &second.entry, 2);
	delete_registration(identifier, TRUE);
	delete_registration(identifier, FALSE);

	SetLastError(0x71);
	TEST_CHECK(install(identifier, base, 128, resolve_entry, &first, L"synthetic-unwind-provider.dll"));
	TEST_CHECK_EQ(0x71, GetLastError());
	first.calls = 0;
	expect_entry(&first, 16, &first.entry, 1);
	delete_registration(identifier, TRUE);
	TEST_CHECK(VirtualFree(code, 0, MEM_RELEASE));
	return 0;
}
