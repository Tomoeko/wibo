#define _WIN32_WINNT 0x0601
#include <limits.h>
#include <stdio.h>
#include <windows.h>

enum { kErrorSeed = 17185 };

static unsigned failures;

static void check(int condition, const char *caseName) {
	if (!condition) {
		++failures;
		fprintf(stderr, "thread priority mismatch: %s\n", caseName);
	}
}

static void expectGet(HANDLE thread, int expected, DWORD expectedError, const char *caseName) {
	SetLastError(kErrorSeed);
	const int result = GetThreadPriority(thread);
	const DWORD error = GetLastError();
	check(result == expected && error == expectedError, caseName);
}

static void expectSet(HANDLE thread, int priority, BOOL expected, DWORD expectedError, const char *caseName) {
	SetLastError(kErrorSeed);
	const BOOL result = SetThreadPriority(thread, priority);
	const DWORD error = GetLastError();
	check((result != FALSE) == (expected != FALSE) && error == expectedError, caseName);
}

static DWORD WINAPI waitForStop(void *argument) {
	return WaitForSingleObject((HANDLE)argument, 5000) == WAIT_OBJECT_0 ? 7 : 8;
}

static void checkReduced(HANDLE original, DWORD access, int value, DWORD getError, BOOL setSuccess, DWORD setError,
						 const char *caseName) {
	HANDLE reduced = NULL;
	if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &reduced, access, FALSE, 0) || !reduced) {
		check(0, caseName);
		return;
	}
	expectGet(reduced, value, getError, caseName);
	expectSet(reduced, 1, setSuccess, setError, caseName);
	check(CloseHandle(reduced), "close reduced handle");
}

int main(void) {
	const int supported[] = {-2, -1, 0, 1, 2};
	expectGet(GetCurrentThread(), 0, kErrorSeed, "pseudo initial");
	for (unsigned index = 0; index < sizeof(supported) / sizeof(supported[0]); ++index) {
		expectSet(GetCurrentThread(), supported[index], TRUE, kErrorSeed, "pseudo set");
		expectGet(GetCurrentThread(), supported[index], kErrorSeed, "pseudo get");
	}
	expectSet(GetCurrentThread(), 3, FALSE, ERROR_INVALID_PARAMETER, "invalid level three");
	expectSet(GetCurrentThread(), INT_MAX, FALSE, ERROR_INVALID_PARAMETER, "invalid level maximum");
	expectGet(GetCurrentThread(), 2, kErrorSeed, "invalid level preserves value");
	expectSet(GetCurrentThread(), 0, TRUE, kErrorSeed, "pseudo restore");

	HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!stop) {
		fprintf(stderr, "worker stop event could not be created\n");
		return 1;
	}
	HANDLE thread = CreateThread(NULL, 0, waitForStop, stop, 0, NULL);
	if (!thread) {
		CloseHandle(stop);
		fprintf(stderr, "worker thread could not be created\n");
		return 1;
	}
	expectGet(thread, 0, kErrorSeed, "created initial");
	for (unsigned index = 0; index < sizeof(supported) / sizeof(supported[0]); ++index) {
		expectSet(thread, supported[index], TRUE, kErrorSeed, "created set");
		expectGet(thread, supported[index], kErrorSeed, "created get");
	}
	expectSet(thread, 3, FALSE, ERROR_INVALID_PARAMETER, "created invalid level");
	expectGet(thread, 2, kErrorSeed, "created invalid level preserves value");
	expectSet(thread, 0, TRUE, kErrorSeed, "created restore");

	checkReduced(thread, THREAD_QUERY_INFORMATION, 0, kErrorSeed, FALSE, ERROR_ACCESS_DENIED, "query rights");
	checkReduced(thread, THREAD_QUERY_LIMITED_INFORMATION, 0, kErrorSeed, FALSE, ERROR_ACCESS_DENIED,
				 "limited query rights");
	checkReduced(thread, THREAD_SET_INFORMATION, THREAD_PRIORITY_ERROR_RETURN, ERROR_ACCESS_DENIED, TRUE, kErrorSeed,
				 "set rights");
	expectGet(thread, 1, kErrorSeed, "set handle shares priority");
	expectSet(thread, 0, TRUE, kErrorSeed, "restore after reduced handle");
	checkReduced(thread, THREAD_SET_LIMITED_INFORMATION, THREAD_PRIORITY_ERROR_RETURN, ERROR_ACCESS_DENIED, FALSE,
				 ERROR_ACCESS_DENIED, "limited set rights");
	checkReduced(thread, 0, THREAD_PRIORITY_ERROR_RETURN, ERROR_ACCESS_DENIED, FALSE, ERROR_ACCESS_DENIED,
				 "zero rights");

	HANDLE duplicate = NULL;
	if (DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &duplicate, 0, FALSE,
						DUPLICATE_SAME_ACCESS) &&
		duplicate) {
		expectSet(duplicate, 2, TRUE, kErrorSeed, "same-access duplicate set");
		expectGet(thread, 2, kErrorSeed, "original sees duplicate value");
		expectSet(duplicate, 0, TRUE, kErrorSeed, "same-access duplicate restore");
		check(CloseHandle(duplicate), "close duplicate");
	} else
		check(0, "duplicate same access");

	HANDLE invalid = (HANDLE)(UINT_PTR)0x1234;
	expectGet(invalid, THREAD_PRIORITY_ERROR_RETURN, ERROR_INVALID_HANDLE, "invalid get");
	expectSet(invalid, 2, FALSE, ERROR_INVALID_HANDLE, "invalid set");
	check(SetEvent(stop), "release worker");
	DWORD joined = WaitForSingleObject(thread, 5000);
	if (joined != WAIT_OBJECT_0) {
		check(0, "join worker");
		TerminateThread(thread, 9);
		joined = WaitForSingleObject(thread, 5000);
	}
	if (joined == WAIT_OBJECT_0) {
		expectGet(thread, 0, kErrorSeed, "terminated retained value");
		expectSet(thread, 1, TRUE, kErrorSeed, "terminated set");
		expectGet(thread, 1, kErrorSeed, "terminated get");
	} else
		check(0, "reap worker");
	check(CloseHandle(thread), "close thread");
	check(CloseHandle(stop), "close stop event");
	printf("software_checks_failed=%u\n", failures);
	return failures ? 1 : 0;
}
