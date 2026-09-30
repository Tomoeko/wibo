#include "test_assert.h"
#include <windows.h>

#ifdef _WIN64
typedef BOOLEAN(WINAPI *add_table_fn)(PRUNTIME_FUNCTION, DWORD, DWORD64);
typedef BOOLEAN(WINAPI *delete_table_fn)(PRUNTIME_FUNCTION);
typedef PRUNTIME_FUNCTION(WINAPI *lookup_fn)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);

static void expect_entry(lookup_fn lookup, BYTE *code, DWORD offset, PRUNTIME_FUNCTION expected) {
	DWORD64 imageBase = 0x12345678;
	SetLastError(0x71);
	TEST_CHECK(lookup((DWORD64)(ULONG_PTR)code + offset, &imageBase, NULL) == expected);
	TEST_CHECK_U64_EQ(expected ? (ULONG_PTR)code : 0, imageBase);
	TEST_CHECK_EQ(0x71, GetLastError());
}
#endif

int main(void) {
	HMODULE native = GetModuleHandleA("ntdll.dll");
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(native != NULL && kernel != NULL);
#ifdef _WIN64
	add_table_fn addNative = (add_table_fn)(void *)GetProcAddress(native, "RtlAddFunctionTable");
	add_table_fn addKernel = (add_table_fn)(void *)GetProcAddress(kernel, "RtlAddFunctionTable");
	delete_table_fn deleteNative = (delete_table_fn)(void *)GetProcAddress(native, "RtlDeleteFunctionTable");
	delete_table_fn deleteKernel = (delete_table_fn)(void *)GetProcAddress(kernel, "RtlDeleteFunctionTable");
	lookup_fn lookupNative = (lookup_fn)(void *)GetProcAddress(native, "RtlLookupFunctionEntry");
	lookup_fn lookupKernel = (lookup_fn)(void *)GetProcAddress(kernel, "RtlLookupFunctionEntry");
	TEST_CHECK(addNative && addKernel && deleteNative && deleteKernel && lookupNative && lookupKernel);

	BYTE *code = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	TEST_CHECK(code != NULL);
	code[128] = 1;
	RUNTIME_FUNCTION entries[2] = {{16, 32, 128}, {48, 80, 128}};
	add_table_fn additions[] = {addNative, addKernel};
	delete_table_fn removals[] = {deleteKernel, deleteNative};
	lookup_fn lookups[] = {lookupNative, lookupKernel};
	for (unsigned registration = 0; registration < 2; ++registration) {
		SetLastError(0x71);
		TEST_CHECK(additions[registration](entries, 2, (DWORD64)(ULONG_PTR)code));
		TEST_CHECK_EQ(0x71, GetLastError());
		for (unsigned lookup = 0; lookup < 2; ++lookup) {
			expect_entry(lookups[lookup], code, 16, &entries[0]);
			expect_entry(lookups[lookup], code, 31, &entries[0]);
			expect_entry(lookups[lookup], code, 48, &entries[1]);
			expect_entry(lookups[lookup], code, 79, &entries[1]);
			expect_entry(lookups[lookup], code, 80, NULL);
		}
		SetLastError(0x71);
		TEST_CHECK(removals[registration](entries));
		TEST_CHECK_EQ(0x71, GetLastError());
		for (unsigned lookup = 0; lookup < 2; ++lookup)
			expect_entry(lookups[lookup], code, 16, NULL);
	}
	SetLastError(0x71);
	TEST_CHECK(addNative(NULL, 0, (DWORD64)(ULONG_PTR)code));
	TEST_CHECK_EQ(0x71, GetLastError());
	TEST_CHECK(deleteKernel(NULL));
	TEST_CHECK(VirtualFree(code, 0, MEM_RELEASE));
#else
	TEST_CHECK(GetProcAddress(native, "RtlAddFunctionTable") == NULL);
	TEST_CHECK(GetProcAddress(kernel, "RtlAddFunctionTable") == NULL);
#endif
	return 0;
}
