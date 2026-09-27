#include "test_assert.h"
#include <shlwapi.h>
#include <wchar.h>
#include <windows.h>

static void testFullPaths(void) {
	const struct {
		const char *input, *expected;
	} cases[] = {{"Z:\\wibo-path\\folder", "Z:\\wibo-path\\folder"},
				 {"Z:\\wibo-path\\folder\\.", "Z:\\wibo-path\\folder"},
				 {"Z:\\wibo-path\\folder\\..", "Z:\\wibo-path"},
				 {"Z:\\wibo-path\\folder\\.\\", "Z:\\wibo-path\\folder\\"},
				 {"Z:\\wibo-path\\folder\\..\\", "Z:\\wibo-path\\"},
				 {"Z:\\.", "Z:\\"},
				 {"Z:\\..", "Z:\\"},
				 {"Z:\\..\\..", "Z:\\"},
				 {"Z:\\", "Z:\\"},
				 {"Z:\\wibo-path\\folder\\", "Z:\\wibo-path\\folder\\"},
				 {"Z:\\wibo-path\\folder/.", "Z:\\wibo-path\\folder"},
				 {"Z:\\wibo-path\\folder/..", "Z:\\wibo-path"}};
	for (unsigned i = 0; i != sizeof(cases) / sizeof(*cases); ++i) {
		char output[MAX_PATH], *part = NULL;
		const char *lastSlash = strrchr(cases[i].expected, '\\');
		const BOOL hasPart = lastSlash && lastSlash[1];
		DWORD length = GetFullPathNameA(cases[i].input, sizeof(output), output, &part);
		TEST_CHECK_EQ(strlen(cases[i].expected), length);
		TEST_CHECK_STR_EQ(cases[i].expected, output);
		if (hasPart) {
			TEST_CHECK(part != NULL);
			TEST_CHECK_EQ(lastSlash + 1 - cases[i].expected, part - output);
		} else
			TEST_CHECK(part == NULL);

		WCHAR inputW[MAX_PATH], expectedW[MAX_PATH], outputW[MAX_PATH], *partW = NULL;
		TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, cases[i].input, -1, inputW, MAX_PATH));
		TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, cases[i].expected, -1, expectedW, MAX_PATH));
		length = GetFullPathNameW(inputW, MAX_PATH, outputW, &partW);
		TEST_CHECK_EQ(wcslen(expectedW), length);
		TEST_CHECK_MSG(wcscmp(expectedW, outputW) == 0, "Unexpected full path for case %u", i);
		if (hasPart) {
			TEST_CHECK(partW != NULL);
			TEST_CHECK_EQ(lastSlash + 1 - cases[i].expected, partW - outputW);
		} else
			TEST_CHECK(partW == NULL);
	}
}

int main(void) {
	testFullPaths();
	const struct {
		const WCHAR *input, *expected;
	} cases[] = {{L"", L"\\"},
				 {L".", L"."},
				 {L"..", L".."},
				 {L"...", L"..."},
				 {L"a\\", L"a\\"},
				 {L"a\\.", L"a\\."},
				 {L"a\\..", L"\\"},
				 {L"a\\..\\..\\b", L"\\b"},
				 {L"..\\b", L"..\\b"},
				 {L"a\\.\\b", L"a\\b"},
				 {L"a\\..\\b", L"\\b"},
				 {L"a/b/../c", L"a/b/../c"},
				 {L"a\\\\b", L"a\\\\b"},
				 {L"\\a\\..", L"\\"},
				 {L"\\..\\b", L"\\b"},
				 {L"C:", L"C:\\"},
				 {L"C:\\foo\\..\\..", L"C:\\"},
				 {L"C:\\name_1\\.\\name_2\\..\\name_3", L"C:\\name_1\\name_3"},
				 {L"\\\\server\\share\\a\\..\\..\\..", L"\\\\server"},
				 {L"\\\\server\\share\\..", L"\\\\server"},
				 {L"\\\\server\\..", L"\\\\server"},
				 {L"a\\b...\\c", L"a\\b...\\c"},
				 {L".\\a", L"a"},
				 {L"..\\..", L"\\"},
				 {L"C:\\\x03A9\\..\\\x4E00", L"C:\\\x4E00"}};
	struct {
		WCHAR output[MAX_PATH];
		DWORD sentinel;
	} storage;
	for (unsigned i = 0; i != sizeof(cases) / sizeof(*cases); ++i) {
		memset(&storage, getenv("WIBO_FIXTURE_RUNTIME") ? 0x55 : 0, sizeof(storage));
		WCHAR input[MAX_PATH] = {0};
		for (unsigned j = 0; cases[i].input[j]; ++j)
			input[j] = cases[i].input[j];
		storage.sentinel = 0x12345678;
		SetLastError(777);
		TEST_CHECK(PathCanonicalizeW(storage.output, getenv("WIBO_FIXTURE_RUNTIME") ? cases[i].input : input));
		TEST_CHECK_MSG(wcscmp(storage.output, cases[i].expected) == 0, "Unexpected canonical path for case %u", i);
		TEST_CHECK_EQ(777, GetLastError());
		TEST_CHECK_EQ(0x12345678, storage.sentinel);
	}
	TEST_CHECK(!PathCanonicalizeW(storage.output, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK_EQ(0, storage.output[0]);
	TEST_CHECK(!PathCanonicalizeW(NULL, L"a"));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	WCHAR longest[MAX_PATH];
	for (unsigned i = 0; i != MAX_PATH - 1; ++i)
		longest[i] = L'a';
	longest[MAX_PATH - 1] = 0;
	TEST_CHECK(PathCanonicalizeW(storage.output, longest));
	TEST_CHECK_EQ(MAX_PATH - 1, wcslen(storage.output));
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		WCHAR overlong[MAX_PATH + 1];
		for (unsigned i = 0; i != MAX_PATH; ++i)
			overlong[i] = L'a';
		overlong[MAX_PATH] = 0;
		TEST_CHECK(!PathCanonicalizeW(storage.output, overlong));
		TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
		TEST_CHECK_EQ(0, storage.output[0]);
		TEST_CHECK_EQ(0x12345678, storage.sentinel);
	}
	return 0;
}
