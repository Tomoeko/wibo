#define _WIN32_WINNT 0x0600
#include <windows.h>

#include <stdio.h>
#include <wchar.h>

#include "test_assert.h"

enum { kErrorSeed = 17185 };

static void checkMissingNames(const WCHAR *nameW, const char *nameA) {
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexW(SYNCHRONIZE, FALSE, NULL) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexA(SYNCHRONIZE, FALSE, NULL) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexW(SYNCHRONIZE, FALSE, L"") == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexA(SYNCHRONIZE, FALSE, "") == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexW(SYNCHRONIZE, FALSE, nameW) == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexA(SYNCHRONIZE, FALSE, nameA) == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
}

static void checkOpen(const WCHAR *nameW, const char *nameA, DWORD access, BOOL canOpen, BOOL canWait,
				  BOOL ansi) {
	SetLastError(kErrorSeed);
	HANDLE handle = ansi ? OpenMutexA(access, TRUE, nameA) : OpenMutexW(access, FALSE, nameW);
	DWORD error = GetLastError();
	if (!canOpen) {
		TEST_CHECK(handle == NULL);
		TEST_CHECK_EQ(ERROR_ACCESS_DENIED, error);
		return;
	}
	TEST_CHECK(handle != NULL);
	TEST_CHECK_EQ(kErrorSeed, error);
	DWORD flags = 0;
	TEST_CHECK(GetHandleInformation(handle, &flags));
	TEST_CHECK_EQ(ansi ? HANDLE_FLAG_INHERIT : 0, flags);
	SetLastError(kErrorSeed);
	DWORD waitResult = WaitForSingleObject(handle, 0);
	error = GetLastError();
	TEST_CHECK_EQ(canWait ? WAIT_OBJECT_0 : WAIT_FAILED, waitResult);
	TEST_CHECK_EQ(canWait ? kErrorSeed : ERROR_ACCESS_DENIED, error);
	if (canWait) {
		SetLastError(kErrorSeed);
		TEST_CHECK(ReleaseMutex(handle));
		TEST_CHECK_EQ(kErrorSeed, GetLastError());
	}
	TEST_CHECK(CloseHandle(handle));
}

int main(void) {
	WCHAR nameW[96], differentCase[96];
	char nameA[96];
	int length = snprintf(nameA, sizeof(nameA), "FixtureOpenMutex_%lu", (unsigned long)GetCurrentProcessId());
	TEST_CHECK(length > 0 && (size_t)length < sizeof(nameA));
	for (int index = 0; index <= length; ++index)
		nameW[index] = differentCase[index] = (WCHAR)(unsigned char)nameA[index];
	differentCase[0] = L'f';

	checkMissingNames(nameW, nameA);
	SetLastError(kErrorSeed);
	HANDLE created = CreateMutexExW(NULL, nameW, 0, MUTEX_ALL_ACCESS);
	TEST_CHECK(created != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	const struct {
		DWORD access;
		BOOL canOpen;
		BOOL canWait;
	} cases[] = {{0, FALSE, FALSE},
				 {MUTEX_MODIFY_STATE, TRUE, FALSE},
				 {SYNCHRONIZE, TRUE, TRUE},
				 {MUTEX_ALL_ACCESS, TRUE, TRUE},
				 {GENERIC_READ, TRUE, FALSE},
				 {GENERIC_WRITE, TRUE, FALSE},
				 {GENERIC_EXECUTE, TRUE, TRUE},
				 {GENERIC_ALL, TRUE, TRUE},
				 {MAXIMUM_ALLOWED, TRUE, TRUE},
				 {0x00800000u, TRUE, FALSE}};
	for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		checkOpen(nameW, nameA, cases[index].access, cases[index].canOpen, cases[index].canWait, FALSE);
		checkOpen(nameW, nameA, cases[index].access, cases[index].canOpen, cases[index].canWait, TRUE);
	}
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexW(SYNCHRONIZE, FALSE, differentCase) == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	TEST_CHECK(CloseHandle(created));
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexW(SYNCHRONIZE, FALSE, nameW) == NULL);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());

	HANDLE event = CreateEventW(NULL, FALSE, FALSE, nameW);
	TEST_CHECK(event != NULL);
	SetLastError(kErrorSeed);
	TEST_CHECK(OpenMutexW(SYNCHRONIZE, FALSE, nameW) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(event));
	printf("open_mutex_access_contexts=%lu encodings=2\n", (unsigned long)(sizeof(cases) / sizeof(cases[0])));
	return 0;
}
