#define _WIN32_WINNT 0x0600
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "test_assert.h"

enum { kErrorSeed = 17185 };

static DWORD waitMutex(HANDLE mutex, unsigned kind) {
	switch (kind) {
	case 0:
		return WaitForSingleObject(mutex, 0);
	case 1:
		return WaitForSingleObjectEx(mutex, 0, TRUE);
	case 2:
		return WaitForMultipleObjects(1, &mutex, FALSE, 0);
	default:
		return WaitForMultipleObjectsEx(1, &mutex, TRUE, 0, TRUE);
	}
}

static void checkAccess(DWORD access, BOOL canWait, unsigned index, BOOL initialOwner, BOOL wide) {
	char nameA[96];
	WCHAR nameW[96];
	int length = snprintf(nameA, sizeof(nameA), "FixtureMutexEx_%lu_%u_%ld_%ld", GetCurrentProcessId(), index,
						  (long)initialOwner, (long)wide);
	TEST_CHECK(length > 0 && (size_t)length < sizeof(nameA));
	for (int unit = 0; unit <= length; ++unit)
		nameW[unit] = (WCHAR)(unsigned char)nameA[unit];
	SetLastError(kErrorSeed);
	HANDLE mutex = wide ? CreateMutexExW(NULL, nameW, initialOwner ? CREATE_MUTEX_INITIAL_OWNER : 0, access)
						: CreateMutexExA(NULL, nameA, initialOwner ? CREATE_MUTEX_INITIAL_OWNER : 0, access);
	DWORD error = GetLastError();
	TEST_CHECK(mutex != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, error);
	HANDLE fullAccess = CreateMutexExW(NULL, nameW, 0, MUTEX_ALL_ACCESS);
	TEST_CHECK(fullAccess != NULL);
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	SetLastError(kErrorSeed);
	BOOL released = ReleaseMutex(mutex);
	error = GetLastError();
	TEST_CHECK_EQ(initialOwner, released);
	TEST_CHECK_EQ(initialOwner ? kErrorSeed : ERROR_NOT_OWNER, error);
	for (unsigned kind = 0; kind < 4; ++kind) {
		SetLastError(kErrorSeed);
		DWORD result = waitMutex(mutex, kind);
		error = GetLastError();
		TEST_CHECK_EQ(canWait ? WAIT_OBJECT_0 : WAIT_FAILED, result);
		TEST_CHECK_EQ(canWait ? kErrorSeed : ERROR_ACCESS_DENIED, error);
		if (canWait) {
			SetLastError(kErrorSeed);
			TEST_CHECK(ReleaseMutex(fullAccess));
			TEST_CHECK_EQ(kErrorSeed, GetLastError());
		}
	}
	HANDLE duplicate = NULL;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), mutex, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	TEST_CHECK(duplicate != NULL);
	SetLastError(kErrorSeed);
	DWORD result = WaitForSingleObject(duplicate, 0);
	error = GetLastError();
	TEST_CHECK_EQ(canWait ? WAIT_OBJECT_0 : WAIT_FAILED, result);
	TEST_CHECK_EQ(canWait ? kErrorSeed : ERROR_ACCESS_DENIED, error);
	if (canWait)
		TEST_CHECK(ReleaseMutex(fullAccess));
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(CloseHandle(fullAccess));
	TEST_CHECK(CloseHandle(mutex));
}

static void checkBasic(void) {
	const DWORD flags[] = {0, CREATE_MUTEX_INITIAL_OWNER, 2, 0xffffffffu};
	for (unsigned index = 0; index < sizeof(flags) / sizeof(flags[0]); ++index) {
		for (unsigned wide = 0; wide < 2; ++wide) {
			SetLastError(kErrorSeed);
			HANDLE mutex = wide ? CreateMutexExW(NULL, NULL, flags[index], MUTEX_ALL_ACCESS)
								: CreateMutexExA(NULL, NULL, flags[index], MUTEX_ALL_ACCESS);
			DWORD error = GetLastError();
			TEST_CHECK(mutex != NULL);
			TEST_CHECK_EQ(ERROR_SUCCESS, error);
			SetLastError(kErrorSeed);
			BOOL released = ReleaseMutex(mutex);
			error = GetLastError();
			BOOL owner = (flags[index] & CREATE_MUTEX_INITIAL_OWNER) != 0;
			TEST_CHECK_EQ(owner, released);
			TEST_CHECK_EQ(owner ? kErrorSeed : ERROR_NOT_OWNER, error);
			TEST_CHECK(CloseHandle(mutex));
		}
	}
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
	HANDLE inherited = CreateMutexExW(&attributes, NULL, 0, MUTEX_ALL_ACCESS);
	TEST_CHECK(inherited != NULL);
	DWORD handleFlags = 0;
	TEST_CHECK(GetHandleInformation(inherited, &handleFlags));
	TEST_CHECK_EQ(HANDLE_FLAG_INHERIT, handleFlags);
	TEST_CHECK(CloseHandle(inherited));

	char nameA[96];
	WCHAR nameW[96], differentCase[96];
	int length = snprintf(nameA, sizeof(nameA), "FixtureMutexEx_name_%lu", GetCurrentProcessId());
	TEST_CHECK(length > 0 && (size_t)length < sizeof(nameA));
	for (int index = 0; index <= length; ++index)
		nameW[index] = differentCase[index] = (WCHAR)(unsigned char)nameA[index];
	differentCase[0] = L'f';
	SetLastError(kErrorSeed);
	HANDLE first = CreateMutexExW(NULL, nameW, 0, MUTEX_ALL_ACCESS);
	TEST_CHECK(first != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	HANDLE second = CreateMutexExA(NULL, nameA, CREATE_MUTEX_INITIAL_OWNER, SYNCHRONIZE);
	TEST_CHECK(second != NULL);
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(!ReleaseMutex(second));
	TEST_CHECK_EQ(ERROR_NOT_OWNER, GetLastError());
	TEST_CHECK(CloseHandle(second));
	HANDLE caseVariant = CreateMutexExW(NULL, differentCase, 0, MUTEX_ALL_ACCESS);
	TEST_CHECK(caseVariant != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	TEST_CHECK(CloseHandle(caseVariant));
	SetLastError(kErrorSeed);
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, nameW);
	TEST_CHECK(event == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(first));
	HANDLE recreated = CreateMutexExW(NULL, nameW, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
	TEST_CHECK(recreated != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(recreated, 0));
	TEST_CHECK(ReleaseMutex(recreated));
	TEST_CHECK(ReleaseMutex(recreated));
	TEST_CHECK(!ReleaseMutex(recreated));
	TEST_CHECK_EQ(ERROR_NOT_OWNER, GetLastError());
	TEST_CHECK(CloseHandle(recreated));
	SetLastError(kErrorSeed);
	HANDLE emptyA = CreateMutexExA(NULL, "", 0, MUTEX_ALL_ACCESS);
	TEST_CHECK(emptyA != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	SetLastError(kErrorSeed);
	HANDLE emptyW = CreateMutexExW(NULL, L"", 0, MUTEX_ALL_ACCESS);
	TEST_CHECK(emptyW != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	TEST_CHECK(CloseHandle(emptyW));
	TEST_CHECK(CloseHandle(emptyA));
}

static void checkUnsupported(void) {
	SECURITY_DESCRIPTOR descriptor = {0};
	descriptor.Revision = SECURITY_DESCRIPTOR_REVISION;
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), &descriptor, FALSE};
	SetLastError(kErrorSeed);
	TEST_CHECK(CreateMutexExW(&attributes, NULL, 0, MUTEX_ALL_ACCESS) == NULL);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(CreateMutexExA(NULL, "FixtureMutex_\x80", 0, MUTEX_ALL_ACCESS) == NULL);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(CreateMutexExW(NULL, L"Local\\FixtureMutex", 0, MUTEX_ALL_ACCESS) == NULL);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	SetLastError(kErrorSeed);
	TEST_CHECK(CreateMutexExW(NULL, NULL, 0, 0x00800000u) == NULL);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	WCHAR longName[261];
	for (unsigned index = 0; index < 260; ++index)
		longName[index] = L'x';
	longName[260] = 0;
	SetLastError(kErrorSeed);
	TEST_CHECK(CreateMutexExW(NULL, longName, 0, MUTEX_ALL_ACCESS) == NULL);
	TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	printf("unsupported_mutex_contexts=5\n");
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "--unsupported") == 0) {
		checkUnsupported();
		return 0;
	}
	TEST_CHECK_EQ(1, argc);
	const struct {
		DWORD access;
		BOOL canWait;
	} cases[] = {{0, FALSE},
				 {MUTEX_MODIFY_STATE, FALSE},
				 {SYNCHRONIZE, TRUE},
				 {MUTEX_ALL_ACCESS, TRUE},
				 {GENERIC_READ, FALSE},
				 {GENERIC_WRITE, FALSE},
				 {GENERIC_EXECUTE, TRUE},
				 {GENERIC_ALL, TRUE},
				 {MAXIMUM_ALLOWED, TRUE}};
	checkBasic();
	for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		for (unsigned owner = 0; owner < 2; ++owner) {
			for (unsigned wide = 0; wide < 2; ++wide)
				checkAccess(cases[index].access, cases[index].canWait, index, (BOOL)owner, (BOOL)wide);
		}
	}
	printf("mutex_access_contexts=36 wait_entry_points=4\n");
	return 0;
}
