#define _WIN32_WINNT 0x0601
// clang-format off: Windows base types must precede winternl.h.
#include <windows.h>
#include <winternl.h>
// clang-format on

#include <stdio.h>
#include <string.h>

#include "test_assert.h"

typedef NTSTATUS(WINAPI *GetDllHandle)(LPCWSTR, ULONG, const UNICODE_STRING *, HMODULE *);

static unsigned wideLength(const WCHAR *value) {
	unsigned length = 0;
	while (value[length])
		++length;
	return length;
}

static void observe(GetDllHandle getHandle, const char *label, const WCHAR *text, unsigned length, LPCWSTR path,
					ULONG flags, HMODULE expected) {
	UNICODE_STRING name = {0};
	name.Length = (USHORT)(length * sizeof(WCHAR));
	name.MaximumLength = (USHORT)((wideLength(text) + 1) * sizeof(WCHAR));
	name.Buffer = (PWSTR)text;
	struct {
		DWORD before;
		HMODULE handle;
		DWORD after;
	} output = {0x12345678, (HMODULE)(ULONG_PTR)0x1234, 0x87654321};
	SetLastError(0x4321);
	NTSTATUS status = getHandle(path, flags, &name, &output.handle);
	DWORD error = GetLastError();
	printf("case=%s status=%08lx lastError=%lu equal=%d unchanged=%d guards=%d\n", label, (unsigned long)status,
		   (unsigned long)error, output.handle == expected, output.handle == (HMODULE)(ULONG_PTR)0x1234,
		   output.before == 0x12345678 && output.after == 0x87654321);
	TEST_CHECK_EQ(0x4321, error);
	TEST_CHECK_EQ(0x12345678, output.before);
	TEST_CHECK_EQ(0x87654321, output.after);
	TEST_CHECK_EQ(expected ? 0 : 0xc0000135, (ULONG)status);
	TEST_CHECK(output.handle == (expected ? expected : (HMODULE)(ULONG_PTR)0x1234));
}

int main(void) {
	HMODULE native = GetModuleHandleW(L"ntdll.dll"), core = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(native && core);
	FARPROC address = GetProcAddress(native, "LdrGetDllHandle");
	GetDllHandle getHandle;
	_Static_assert(sizeof(address) == sizeof(getHandle), "function pointer size");
	TEST_CHECK(address != NULL);
	memcpy(&getHandle, &address, sizeof(getHandle));
	char countedMode[2] = {0};
	int countedNames =
		GetEnvironmentVariableA("WIBO_EXPECT_COUNTED_DLL_NAMES", countedMode, sizeof(countedMode)) == 1 &&
		countedMode[0] == '1';
	observe(getHandle, "core", L"kernel32.dll", 12, NULL, 0, core);
	observe(getHandle, "core-case", L"KERNEL32.DLL", 12, NULL, 0, core);
	observe(getHandle, "core-extension", L"kernel32", 8, NULL, 0, core);
	observe(getHandle, "native", L"ntdll.dll", 9, NULL, 0, native);
	observe(getHandle, "path-ignored-for-loaded", L"kernel32.dll", 12, L"C:\\unavailable-search", 0, core);
	observe(getHandle, "flags", L"kernel32.dll", 12, NULL, 0xffffffff, core);
	observe(getHandle, "missing", L"unloaded-fixture-module.dll", 27, NULL, 0, NULL);
	observe(getHandle, "empty", L"", 0, NULL, 0, NULL);
	observe(getHandle, "counted-tail", L"kernel32.dllsuffix", 12, NULL, 0, countedNames ? core : NULL);
	TEST_CHECK(GetModuleHandleW(L"external_exports.dll") == NULL);
	observe(getHandle, "existing-unloaded", L"external_exports.dll", 20, NULL, 0, NULL);
	TEST_CHECK(GetModuleHandleW(L"external_exports.dll") == NULL);
	HMODULE external = LoadLibraryW(L"external_exports.dll");
	TEST_CHECK(external != NULL);
	observe(getHandle, "external", L"external_exports.dll", 20, NULL, 0, external);
	observe(getHandle, "external-extension", L"external_exports", 16, NULL, 0, external);
	WCHAR fullName[MAX_PATH];
	DWORD length = GetModuleFileNameW(external, fullName, MAX_PATH);
	TEST_CHECK(length && length < MAX_PATH);
	observe(getHandle, "external-full-path", fullName, length, NULL, 0, external);
	observe(getHandle, "wrong-full-path", L"C:\\unavailable-search\\external_exports.dll", 42, NULL, 0, NULL);
	TEST_CHECK(FreeLibrary(external));
	printf("case=no-added-reference unloaded=%d\n", GetModuleHandleW(L"external_exports.dll") == NULL);
	TEST_CHECK(GetModuleHandleW(L"external_exports.dll") == NULL);
	observe(getHandle, "external-after-free", L"external_exports.dll", 20, NULL, 0, NULL);
	return 0;
}
