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
};

static FindStringFn findString;
static const int untouchedLength = -9137;
static const WCHAR source[] = {'x', 'A', 'b', 'x', 'A', 'b'};
static const WCHAR value[] = {'A', 'b'};

_Static_assert(sizeof(WCHAR) == 2 && sizeof(int) == 4, "Search transport uses UTF-16 and 32-bit output");

static struct CallResult callFind(const char *mode, DWORD flags, LPCWSTR text, int count, LPCWSTR needle,
								  int needleCount, BOOL withLength, LPNLSVERSIONINFO version, LPVOID reserved,
								  LPARAM handle) {
	struct {
		uint32_t before;
		int length;
		uint32_t after;
	} guarded = {UINT32_C(0x13579bdf), untouchedLength, UINT32_C(0x2468ace0)};
	TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NLS_FIND_RESPONSE", mode));
	SetLastError(0x4321);
	struct CallResult result;
	result.index = findString(L"en-US", flags, text, count, needle, needleCount, withLength ? &guarded.length : NULL,
							  version, reserved, handle);
	result.error = GetLastError();
	result.length = guarded.length;
	printf("mode=%s index=%d length=%d error=%lu\n", mode, result.index, result.length, (unsigned long)result.error);
	TEST_CHECK_EQ(UINT32_C(0x13579bdf), guarded.before);
	TEST_CHECK_EQ(UINT32_C(0x2468ace0), guarded.after);
	if (!withLength)
		TEST_CHECK_EQ(untouchedLength, result.length);
	return result;
}

static struct CallResult basic(const char *mode, BOOL withLength) {
	return callFind(mode, FIND_FROMSTART, source, 6, value, 2, withLength, NULL, NULL, 0);
}

static void checkResult(struct CallResult result, int index, int length, DWORD error) {
	TEST_CHECK_EQ(index, result.index);
	TEST_CHECK_EQ(length, result.length);
	TEST_CHECK_EQ(error, result.error);
}

static void checkFailure(struct CallResult result, DWORD error) { checkResult(result, -1, untouchedLength, error); }

static void testResponses(void) {
	// These tuples protect the protocol; the separate native fixture verifies collation.
	checkResult(basic("match", TRUE), 1, 2, 0x4321);
	checkResult(basic("optional", FALSE), 1, untouchedLength, 0x4321);
	checkResult(basic("cleared-error", TRUE), 1, 2, 0);
	checkResult(callFind("zero-index", FIND_FROMSTART, value, 2, value, 2, TRUE, NULL, NULL, 0), 0, 2, 0x4321);
	checkResult(callFind("terminated", FIND_FROMEND, L"Ab--Ab", -1, L"Ab", -1, TRUE, NULL, NULL, 0), 4, 2, 0x4321);
	checkResult(callFind("no-match", FIND_FROMSTART, L"xyz", 3, value, 2, TRUE, NULL, NULL, 0), -1, untouchedLength,
				0x4321);
	checkResult(callFind("no-match-zero", FIND_FROMSTART, L"xyz", 3, value, 2, TRUE, NULL, NULL, 0), -1,
				untouchedLength, 0);
	checkFailure(basic("failed", TRUE), ERROR_ACCESS_DENIED);
	checkFailure(basic("unavailable", TRUE), ERROR_NOT_SUPPORTED);
	const char *malformed[] = {"failed-with-data", "truncated",		 "trailing",	  "bad-magic",
							   "bad-version",	   "negative-index", "large-index",	  "large-length",
							   "negative-length",  "bad-presence",	 "missing-found", "absent-with-value"};
	for (unsigned index = 0; index < sizeof(malformed) / sizeof(malformed[0]); ++index)
		checkFailure(basic(malformed[index], TRUE), ERROR_INVALID_DATA);
	checkFailure(basic("unexpected-found", FALSE), ERROR_INVALID_DATA);
	checkFailure(callFind("failure-found", FIND_FROMSTART, L"xyz", 3, value, 2, TRUE, NULL, NULL, 0),
				 ERROR_INVALID_DATA);
}

static void testLocalLimits(void) {
	const DWORD unsupportedFlags[] = {0, FIND_FROMSTART | FIND_FROMEND, FIND_FROMSTART | 0x80000000,
									  FIND_FROMSTART | NORM_IGNOREWIDTH};
	for (unsigned index = 0; index < sizeof(unsupportedFlags) / sizeof(unsupportedFlags[0]); ++index)
		checkFailure(callFind("match", unsupportedFlags[index], source, 6, value, 2, TRUE, NULL, NULL, 0),
					 ERROR_NOT_SUPPORTED);
	NLSVERSIONINFO version = {0};
	version.dwNLSVersionInfoSize = sizeof(version);
	checkFailure(callFind("match", FIND_FROMSTART, source, 6, value, 2, TRUE, &version, NULL, 0), ERROR_NOT_SUPPORTED);
	checkFailure(callFind("match", FIND_FROMSTART, source, 6, value, 2, TRUE, NULL, &version, 0), ERROR_NOT_SUPPORTED);
	checkFailure(callFind("match", FIND_FROMSTART, source, 6, value, 2, TRUE, NULL, NULL, 1), ERROR_NOT_SUPPORTED);
	checkFailure(callFind("match", FIND_FROMSTART, source, 0, value, 2, TRUE, NULL, NULL, 0), ERROR_INVALID_PARAMETER);
	checkFailure(callFind("match", FIND_FROMSTART, source, 6, value, -2, TRUE, NULL, NULL, 0), ERROR_INVALID_PARAMETER);
	checkFailure(callFind("match", FIND_FROMSTART, NULL, 6, value, 2, TRUE, NULL, NULL, 0), ERROR_INVALID_PARAMETER);
	static WCHAR large[20000];
	for (unsigned index = 0; index < sizeof(large) / sizeof(large[0]); ++index)
		large[index] = 'A';
	checkFailure(callFind("match", FIND_FROMSTART, large, 20000, value, 2, TRUE, NULL, NULL, 0),
				 ERROR_NOT_ENOUGH_MEMORY);
	checkFailure(callFind("match", FIND_FROMSTART, large, -1, value, 2, TRUE, NULL, NULL, 0), ERROR_NOT_ENOUGH_MEMORY);
	checkFailure(callFind("match", FIND_FROMSTART, large, 8192, large + 8192, 8192, TRUE, NULL, NULL, 0),
				 ERROR_NOT_ENOUGH_MEMORY);
	char provider[32768];
	DWORD length = GetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER", provider, sizeof(provider));
	TEST_CHECK(length > 0 && length < sizeof(provider));
	TEST_CHECK(SetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER", NULL));
	checkFailure(basic("match", TRUE), ERROR_NOT_SUPPORTED);
	TEST_CHECK(SetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER", provider));
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "FindNLSStringEx");
	_Static_assert(sizeof(exported) == sizeof(findString), "Resolved entry pointer width");
	memcpy(&findString, &exported, sizeof(findString));
	TEST_CHECK(findString != NULL);
	// Each response mode gets a fresh helper environment within this one guest run.
	TEST_CHECK(SetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER_PERSISTENT", NULL));
	testResponses();
	testLocalLimits();
	TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NLS_FIND_RESPONSE", NULL));
	return 0;
}
