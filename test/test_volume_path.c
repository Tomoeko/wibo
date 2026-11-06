#define _WIN32_WINNT 0x0601
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

typedef BOOL(WINAPI *VolumePathFn)(const WCHAR *path, WCHAR *output, DWORD capacity);

static void check_root(VolumePathFn query, const WCHAR *path, const WCHAR *expected) {
	WCHAR output[32];
	for (unsigned i = 0; i < 32; ++i)
		output[i] = 0x7315;
	SetLastError(0x9876);
	TEST_CHECK(query(path, output, 32));
	TEST_CHECK(_wcsicmp(output, expected) == 0);
	TEST_CHECK_EQ(0x9876, GetLastError());
	TEST_CHECK_EQ(0x7315, output[wcslen(expected) + 1]);
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC procedure = GetProcAddress(kernel, "GetVolumePathNameW");
	TEST_CHECK(procedure != NULL);
	VolumePathFn query;
	TEST_CHECK(sizeof(query) == sizeof(procedure));
	memcpy(&query, &procedure, sizeof(query));
	check_root(query, L"C:\\", L"C:\\");
	check_root(query, L"c:\\missing-volume-path-test\\file", L"C:\\");
	check_root(query, L"Z:\\", L"Z:\\");
	check_root(query, L"\\\\?\\C:\\missing-volume-path-test", L"\\\\?\\C:\\");
	check_root(query, L"C:\\unicode-\x00e9\\file", L"C:\\");
	WCHAR output[8];
	for (DWORD capacity = 0; capacity < 3; ++capacity) {
		for (unsigned i = 0; i < 8; ++i)
			output[i] = 0x7315;
		SetLastError(0x9876);
		TEST_CHECK(!query(L"C:\\", output, capacity));
		TEST_CHECK_EQ(capacity == 0 ? ERROR_INVALID_PARAMETER : ERROR_FILENAME_EXCED_RANGE, GetLastError());
		TEST_CHECK_EQ(0x7315, output[capacity]);
	}
	TEST_CHECK(query(L"C:", output, 3));
	TEST_CHECK(_wcsicmp(output, L"C:") == 0);
	TEST_CHECK(query(L"C:", output, 4));
	TEST_CHECK(_wcsicmp(output, L"C:\\") == 0);
	SetLastError(0x9876);
	TEST_CHECK(!query(L"", output, 8));
	DWORD emptyError = GetLastError();
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		TEST_CHECK_EQ(ERROR_SUCCESS, emptyError);
	} else {
		TEST_CHECK(emptyError == ERROR_SUCCESS || emptyError == ERROR_INVALID_PARAMETER);
		if (emptyError != ERROR_SUCCESS)
			fprintf(stderr, "Baseline empty-name last-error variant: %lu\n", (unsigned long)emptyError);
	}
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		const WCHAR *unsupported[] = {L"relative", L"\\\\server\\share", L"D:\\missing"};
		for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
			output[0] = 0x7315;
			TEST_CHECK(!query(unsupported[i], output, 8));
			TEST_CHECK_EQ(i == 2 ? ERROR_INVALID_DRIVE : ERROR_NOT_SUPPORTED, GetLastError());
			TEST_CHECK_EQ(0x7315, output[0]);
		}
		WCHAR longPath[32769];
		longPath[0] = 'C';
		longPath[1] = ':';
		longPath[2] = '\\';
		for (unsigned i = 3; i < 32768; ++i)
			longPath[i] = 'a';
		longPath[32768] = 0;
		TEST_CHECK(!query(longPath, output, 8));
		TEST_CHECK_EQ(ERROR_FILENAME_EXCED_RANGE, GetLastError());
		longPath[32767] = 0;
		check_root(query, longPath, L"C:\\");
		TEST_CHECK(!query(NULL, output, 8));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
		TEST_CHECK(!query(L"C:\\", NULL, 8));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	}
	return 0;
}
