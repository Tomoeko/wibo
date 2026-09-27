#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#include <windows.h>

#include <stdint.h>
#include <string.h>

#include "test_assert.h"

typedef int(WINAPI *FindStringFn)(LPCWSTR, DWORD, LPCWSTR, int, LPCWSTR, int, LPINT, LPNLSVERSIONINFO, LPVOID, LPARAM);

struct CallResult {
	int index;
	int length;
	DWORD error;
	BOOL requestedLength;
};

static FindStringFn findString;
static const int untouchedLength = -9137;

_Static_assert(sizeof(WCHAR) == 2 && sizeof(int) == 4, "Native search uses UTF-16 and 32-bit integers");

static struct CallResult callFind(const char *name, LPCWSTR locale, DWORD flags, LPCWSTR source, int sourceCount,
								  LPCWSTR value, int valueCount, BOOL withLength) {
	struct {
		uint32_t before;
		int length;
		uint32_t after;
	} guarded = {UINT32_C(0x13579bdf), untouchedLength, UINT32_C(0x2468ace0)};
	SetLastError(0x4321);
	struct CallResult result;
	result.index = findString(locale, flags, source, sourceCount, value, valueCount,
							  withLength ? &guarded.length : NULL, NULL, NULL, 0);
	result.error = GetLastError();
	result.length = guarded.length;
	result.requestedLength = withLength;
	printf("%s flags=%lx source=%d value=%d length-output=%u index=%d found=%d error=%lu\n", name, (unsigned long)flags,
		   sourceCount, valueCount, (unsigned)withLength, result.index, result.length, (unsigned long)result.error);
	TEST_CHECK_EQ(UINT32_C(0x13579bdf), guarded.before);
	TEST_CHECK_EQ(UINT32_C(0x2468ace0), guarded.after);
	if (!withLength)
		TEST_CHECK_EQ(untouchedLength, result.length);
	return result;
}

static void checkMatch(struct CallResult result, int expectedIndex, int expectedLength) {
	TEST_CHECK_EQ(expectedIndex, result.index);
	if (result.requestedLength)
		TEST_CHECK_EQ(expectedLength, result.length);
}

static void checkNoMatch(struct CallResult result) {
	TEST_CHECK_EQ(-1, result.index);
	TEST_CHECK_EQ(untouchedLength, result.length);
	// The documented no-match error is zero; record the backend value separately.
}

static void checkBadCount(struct CallResult result) {
	checkNoMatch(result);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, result.error);
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "FindNLSStringEx");
	_Static_assert(sizeof(exported) == sizeof(findString), "Resolved entry pointer width");
	memcpy(&findString, &exported, sizeof(findString));
	TEST_CHECK(findString != NULL);

	// These arrays intentionally have no terminator: explicit counts own their extent.
	const WCHAR source[] = {'x', 'A', 'b', 'x', 'A', 'b'};
	const WCHAR exact[] = {'A', 'b'};
	const WCHAR folded[] = {'a', 'B'};
	const WCHAR edge[] = {'X', 'a', 'B'};
	checkMatch(callFind("counted-exact", L"en-US", FIND_FROMSTART, source, 6, exact, 2, TRUE), 1, 2);
	checkMatch(callFind("counted-fold-start", L"en-US", FIND_FROMSTART | NORM_IGNORECASE, source, 6, folded, 2, TRUE),
			   1, 2);
	checkMatch(callFind("counted-fold-end", L"en-US", FIND_FROMEND | NORM_IGNORECASE, source, 6, folded, 2, TRUE), 4,
			   2);
	checkMatch(callFind("prefix", L"en-US", FIND_STARTSWITH | NORM_IGNORECASE, source, 6, edge, 3, TRUE), 0, 3);
	checkMatch(callFind("suffix", L"en-US", FIND_ENDSWITH | NORM_IGNORECASE, source, 6, edge, 3, TRUE), 3, 3);
	checkMatch(callFind("optional-length", L"en-US", FIND_FROMSTART, source, 6, exact, 2, FALSE), 1, 2);
	checkMatch(callFind("terminated", L"en-US", FIND_FROMEND, L"Ab--Ab", -1, L"Ab", -1, TRUE), 4, 2);
	checkMatch(callFind("linguistic-case", L"en-US", FIND_FROMSTART | NORM_IGNORECASE | NORM_LINGUISTIC_CASING, source,
						6, folded, 2, TRUE),
			   1, 2);
	checkMatch(callFind("invariant", LOCALE_NAME_INVARIANT, FIND_FROMSTART, source, 6, exact, 2, TRUE), 1, 2);
	checkMatch(callFind("user-default", LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART, source, 6, exact, 2, TRUE), 1, 2);
	checkMatch(callFind("system-default", LOCALE_NAME_SYSTEM_DEFAULT, FIND_FROMSTART, source, 6, exact, 2, TRUE), 1, 2);

	checkNoMatch(callFind("case-sensitive-miss", L"en-US", FIND_FROMSTART, source, 6, folded, 2, TRUE));
	checkNoMatch(callFind("no-match", L"en-US", FIND_FROMSTART, L"xyz", 3, exact, 2, TRUE));
	checkNoMatch(callFind("prefix-miss", L"en-US", FIND_STARTSWITH, source, 6, exact, 2, TRUE));
	checkNoMatch(callFind("suffix-miss", L"en-US", FIND_ENDSWITH, source, 4, exact, 2, TRUE));
	checkNoMatch(callFind("empty-terminated-source", L"en-US", FIND_FROMSTART, L"", -1, exact, 2, TRUE));
	checkBadCount(callFind("zero-source", L"en-US", FIND_FROMSTART, source, 0, exact, 2, TRUE));
	checkBadCount(callFind("zero-value", L"en-US", FIND_FROMSTART, source, 6, exact, 0, TRUE));
	checkBadCount(callFind("negative-source", L"en-US", FIND_FROMSTART, source, -2, exact, 2, TRUE));
	checkBadCount(callFind("negative-value", L"en-US", FIND_FROMSTART, source, 6, exact, -2, TRUE));
	checkBadCount(callFind("null-source", L"en-US", FIND_FROMSTART, NULL, 1, exact, 2, TRUE));
	checkBadCount(callFind("null-value", L"en-US", FIND_FROMSTART, source, 6, NULL, 1, TRUE));

	// Observe known backend discrepancies without declaring them Windows-compatible.
	printf("documented-observation: flags=0 defaults to first match; invalid flags fail with error 1004\n");
	callFind("default-direction-observation", L"en-US", 0, L"Ab--Ab", 6, exact, 2, TRUE);
	callFind("unknown-flags-observation", L"en-US", FIND_FROMSTART | 0x80000000, source, 6, exact, 2, TRUE);
	callFind("combined-direction-observation", L"en-US", FIND_FROMSTART | FIND_FROMEND, source, 6, exact, 2, TRUE);
	return 0;
}
