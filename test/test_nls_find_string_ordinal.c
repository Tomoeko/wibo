#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include <windows.h>

#include <stdint.h>
#include <string.h>

#include "test_assert.h"

typedef int(WINAPI *FindStringOrdinalFn)(DWORD, LPCWSTR, int, LPCWSTR, int, BOOL);

static FindStringOrdinalFn findStringOrdinal;

static void checkSearch(const char *name, DWORD flags, LPCWSTR source, int sourceCount, LPCWSTR value, int valueCount,
						BOOL ignoreCase, int expected) {
	SetLastError(0x4321);
	const int index = findStringOrdinal(flags, source, sourceCount, value, valueCount, ignoreCase);
	const DWORD error = GetLastError();
	printf("%s index=%d error=%lu\n", name, index, (unsigned long)error);
	if (expected != INT32_MIN)
		TEST_CHECK_EQ(expected, index);
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "FindStringOrdinal");
	_Static_assert(sizeof(exported) == sizeof(findStringOrdinal), "Resolved entry pointer width");
	memcpy(&findStringOrdinal, &exported, sizeof(findStringOrdinal));
	TEST_CHECK(findStringOrdinal != NULL);
	TEST_CHECK(GetProcAddress(kernel, "FindStringOrdinalA") == NULL);

	const WCHAR source[] = {'x', 'A', 'b', 'x', 'A', 'b'};
	const WCHAR exact[] = {'A', 'b'};
	const WCHAR folded[] = {'a', 'B'};
	const WCHAR prefix[] = {'x', 'A'};
	const WCHAR suffix[] = {'A', 'b'};
	checkSearch("forward", FIND_FROMSTART, source, 6, exact, 2, FALSE, 1);
	checkSearch("reverse", FIND_FROMEND, source, 6, exact, 2, FALSE, 4);
	checkSearch("prefix", FIND_STARTSWITH, source, 6, prefix, 2, FALSE, 0);
	checkSearch("suffix", FIND_ENDSWITH, source, 6, suffix, 2, FALSE, 4);
	checkSearch("zero-flags-observation", 0, source, 6, exact, 2, FALSE, INT32_MIN);
	checkSearch("fold-forward", FIND_FROMSTART, source, 6, folded, 2, TRUE, 1);
	checkSearch("fold-reverse", FIND_FROMEND, source, 6, folded, 2, TRUE, 4);
	checkSearch("case-sensitive-miss", FIND_FROMSTART, source, 6, folded, 2, FALSE, -1);
	checkSearch("terminated", FIND_FROMEND, L"Ab--Ab", -1, L"Ab", -1, FALSE, 4);

	const WCHAR embedded[] = {'a', 0, 'B'};
	const WCHAR embeddedValue[] = {0, 'B'};
	checkSearch("embedded-null", FIND_FROMSTART, embedded, 3, embeddedValue, 2, FALSE, 1);
	checkSearch("empty-source", FIND_FROMSTART, source, 0, exact, 2, FALSE, -1);
	checkSearch("empty-value", FIND_FROMSTART, source, 6, exact, 0, FALSE, 0);
	checkSearch("empty-value-reverse-observation", FIND_FROMEND, source, 6, exact, 0, FALSE, INT32_MIN);
	checkSearch("empty-value-suffix-observation", FIND_ENDSWITH, source, 6, exact, 0, FALSE, INT32_MIN);
	checkSearch("empty-terminated", FIND_FROMSTART, L"", -1, L"", -1, FALSE, 0);
	checkSearch("invalid-count", FIND_FROMSTART, source, -2, exact, 2, FALSE, -1);
	checkSearch("null-source", FIND_FROMSTART, NULL, 1, exact, 2, FALSE, -1);
	checkSearch("null-value", FIND_FROMSTART, source, 6, NULL, 2, FALSE, -1);
	checkSearch("invalid-flags", 0x80000000u, source, 6, exact, 2, FALSE, -1);
	checkSearch("multiple-flags", FIND_FROMSTART | FIND_FROMEND, source, 6, exact, 2, FALSE, -1);

	const WCHAR greek[] = {0x0391, 0x03a3};
	const WCHAR greekLower[] = {0x03b1, 0x03c3};
	checkSearch("greek-fold", FIND_FROMSTART, greek, 2, greekLower, 2, TRUE, 0);
	return 0;
}
