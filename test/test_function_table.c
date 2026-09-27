#include "test_assert.h"
#include <windows.h>

typedef DWORD(WINAPI *add_table_fn)(PVOID *, PRUNTIME_FUNCTION, DWORD, DWORD, ULONG_PTR, ULONG_PTR);
typedef VOID(WINAPI *grow_table_fn)(PVOID, DWORD);
typedef VOID(WINAPI *delete_table_fn)(PVOID);
typedef PRUNTIME_FUNCTION(WINAPI *lookup_fn)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);

static lookup_fn lookup;
static BOOL registered;
static void expect_entry(BYTE *code, DWORD offset, PRUNTIME_FUNCTION expected) {
	DWORD64 base = 0x12345678;
	SetLastError(0x71);
	TEST_CHECK(lookup((DWORD64)(ULONG_PTR)code + offset, &base, NULL) == expected);
	if (expected)
		TEST_CHECK_U64_EQ((ULONG_PTR)code, base);
	else
		TEST_CHECK_U64_EQ(registered && offset < 128 ? 0x12345678 : 0, base);
	TEST_CHECK_EQ(0x71, GetLastError());
}

int main(void) {
	HMODULE module = GetModuleHandleW(L"ntdll.dll");
	TEST_CHECK(module != NULL);
	add_table_fn add = (add_table_fn)(ULONG_PTR)GetProcAddress(module, "RtlAddGrowableFunctionTable");
	grow_table_fn grow = (grow_table_fn)(ULONG_PTR)GetProcAddress(module, "RtlGrowFunctionTable");
	delete_table_fn remove = (delete_table_fn)(ULONG_PTR)GetProcAddress(module, "RtlDeleteGrowableFunctionTable");
	lookup = (lookup_fn)(ULONG_PTR)GetProcAddress(module, "RtlLookupFunctionEntry");
	TEST_CHECK(add && grow && remove && lookup);
	BYTE *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	TEST_CHECK(code != NULL);
	code[256] = 1; // Version 1 unwind info with no stack operations.
	RUNTIME_FUNCTION entries[3] = {{16, 32, 256}, {48, 64, 256}, {80, 96, 256}};
	PVOID table = NULL;
	SetLastError(0x71);
	TEST_CHECK_EQ(0, add(&table, entries, 0, 3, (ULONG_PTR)code, (ULONG_PTR)code + 128));
	TEST_CHECK(table != NULL);
	registered = TRUE;
	TEST_CHECK_EQ(0x71, GetLastError());
	expect_entry(code, 16, NULL);
	grow(table, 1);
	expect_entry(code, 15, NULL);
	expect_entry(code, 16, &entries[0]);
	expect_entry(code, 31, &entries[0]);
	expect_entry(code, 32, NULL);
	expect_entry(code, 48, NULL);
	grow(table, 3);
	expect_entry(code, 48, &entries[1]);
	expect_entry(code, 80, &entries[2]);
	expect_entry(code, 96, NULL);
	expect_entry(code, 128, NULL);
	remove(table);
	registered = FALSE;
	expect_entry(code, 16, NULL);
	expect_entry(code, 80, NULL);
	table = NULL;
	TEST_CHECK_EQ(0, add(&table, entries, 3, 3, (ULONG_PTR)code, (ULONG_PTR)code + 128));
	registered = TRUE;
	expect_entry(code, 63, &entries[1]);
	remove(table);
	TEST_CHECK(VirtualFree(code, 0, MEM_RELEASE));
	DWORD64 imageBase = 0;
	PRUNTIME_FUNCTION entry = lookup((DWORD64)(ULONG_PTR)&main, &imageBase, NULL);
	TEST_CHECK(entry != NULL);
	TEST_CHECK_U64_EQ((ULONG_PTR)GetModuleHandleW(NULL), imageBase);
	TEST_CHECK((DWORD64)(ULONG_PTR)&main >= imageBase + entry->BeginAddress);
	TEST_CHECK((DWORD64)(ULONG_PTR)&main < imageBase + entry->EndAddress);
	return 0;
}
