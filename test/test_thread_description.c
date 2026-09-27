#define _WIN32_WINNT 0x0a00
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef HRESULT(WINAPI *SetDescription)(HANDLE, PCWSTR);
typedef HRESULT(WINAPI *GetDescription)(HANDLE, PWSTR *);
static SetDescription setDescription;
static GetDescription getDescription;
static const DWORD seed = 0x4321;
static const uint32_t success = 0x10000000;
static const uint32_t invalidHandle = 0xd0000008;
static const uint32_t accessDenied = 0xd0000022;
static const uint32_t wrongType = 0xd0000024;
static const uint32_t unsupported = 0xd00000bb;

static void setName(HANDLE thread, const WCHAR *name, uint32_t expected) {
	SetLastError(seed);
	const HRESULT result = setDescription(thread, name);
	const DWORD error = GetLastError();
	TEST_CHECK_EQ(expected, (uint32_t)result);
	TEST_CHECK_EQ(seed, error);
}

static WCHAR *getName(HANDLE thread, const WCHAR *expected, uint32_t expectedResult) {
	WCHAR sentinel[2] = {L'X', 0};
	struct {
		uintptr_t before;
		WCHAR *value;
		uintptr_t after;
	} output = {0x5678, sentinel, 0x9abc};
	SetLastError(seed);
	const HRESULT result = getDescription(thread, &output.value);
	const DWORD error = GetLastError();
	TEST_CHECK_EQ(expectedResult, (uint32_t)result);
	TEST_CHECK_EQ(seed, error);
	TEST_CHECK_U64_EQ(0x5678, output.before);
	TEST_CHECK_U64_EQ(0x9abc, output.after);
	if (expectedResult == success) {
		TEST_CHECK(output.value != NULL && output.value != sentinel);
		TEST_CHECK(wcscmp(output.value, expected) == 0);
	} else
		TEST_CHECK(output.value == NULL);
	return output.value;
}

static void checkName(HANDLE thread, const WCHAR *name) {
	WCHAR *value = getName(thread, name, success);
	TEST_CHECK(LocalFree(value) == NULL);
}

static DWORD WINAPI worker(void *parameter) {
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject((HANDLE)parameter, 5000));
	checkName(GetCurrentThread(), L"replacement");
	return 7;
}

static int checkProxy(BOOL localScope) {
	char application[MAX_PATH], command[] = "fixture child";
	const DWORD length = GetModuleFileNameA(NULL, application, sizeof(application));
	if (!length || length >= sizeof(application))
		return 0;
	STARTUPINFOA startup = {0};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {0};
	if (!CreateProcessA(application, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &process))
		return 0;
	const uint32_t expected = localScope ? unsupported : success;
	SetLastError(seed);
	const HRESULT setResult = setDescription(process.hThread, L"proxy-name");
	const DWORD setError = GetLastError();
	WCHAR sentinel[2] = {L'X', 0};
	WCHAR *value = sentinel;
	SetLastError(seed);
	const HRESULT getResult = getDescription(process.hThread, &value);
	const DWORD getError = GetLastError();
	int good =
		(uint32_t)setResult == expected && (uint32_t)getResult == expected && setError == seed && getError == seed;
	if (SUCCEEDED(getResult)) {
		good = good && value && value != sentinel && wcscmp(value, L"proxy-name") == 0;
		if (value && value != sentinel && LocalFree(value) != NULL)
			good = 0;
	} else if (value != NULL)
		good = 0;
	if (ResumeThread(process.hThread) == (DWORD)-1)
		good = 0;
	if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
		good = 0;
		if (!TerminateProcess(process.hProcess, 90) || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
			fprintf(stderr, "Child cleanup did not complete\n");
	} else {
		DWORD code = 0;
		if (!GetExitCodeProcess(process.hProcess, &code) || code)
			good = 0;
		value = sentinel;
		SetLastError(seed);
		const HRESULT afterResult = getDescription(process.hThread, &value);
		const DWORD afterError = GetLastError();
		good = good && (uint32_t)afterResult == expected && afterError == seed;
		if (SUCCEEDED(afterResult)) {
			good = good && value && value != sentinel && value[0] == 0;
			if (value && value != sentinel && LocalFree(value) != NULL)
				good = 0;
		} else if (value != NULL)
			good = 0;
	}
	if (!CloseHandle(process.hThread))
		good = 0;
	if (!CloseHandle(process.hProcess))
		good = 0;
	return good;
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "child") == 0)
		return 0;
	TEST_CHECK(argc == 1 || (argc == 2 && strcmp(argv[1], "local") == 0));
	HMODULE module = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC setter = GetProcAddress(module, "SetThreadDescription");
	FARPROC getter = GetProcAddress(module, "GetThreadDescription");
	TEST_CHECK(setter != NULL && getter != NULL);
	TEST_CHECK(sizeof(setDescription) == sizeof(setter) && sizeof(getDescription) == sizeof(getter));
	memcpy(&setDescription, &setter, sizeof(setDescription));
	memcpy(&getDescription, &getter, sizeof(getDescription));
	checkName(GetCurrentThread(), L"");
	const WCHAR unicode[] = {'F', 0x00e9, 0x03a9, 0xd801, 0xdc00, 0xd800, 0};
	setName(GetCurrentThread(), unicode, success);
	WCHAR *first = getName(GetCurrentThread(), unicode, success);
	WCHAR *second = getName(GetCurrentThread(), unicode, success);
	TEST_CHECK(first != second);
	first[0] = L'X';
	TEST_CHECK(wcscmp(second, unicode) == 0);
	checkName(GetCurrentThread(), unicode);
	TEST_CHECK(LocalFree(first) == NULL && LocalFree(second) == NULL);
	const WCHAR terminated[] = {'A', 0, 'B', 0};
	setName(GetCurrentThread(), terminated, success);
	checkName(GetCurrentThread(), L"A");
	setName(GetCurrentThread(), L"", success);
	checkName(GetCurrentThread(), L"");
	setName(GetCurrentThread(), NULL, success);
	checkName(GetCurrentThread(), L"");
	WCHAR *longName = calloc(32769, sizeof(WCHAR));
	TEST_CHECK(longName != NULL);
	for (unsigned index = 0; index < 32768; ++index)
		longName[index] = (WCHAR)('a' + index % 26);
	longName[1024] = 0;
	setName(GetCurrentThread(), longName, success);
	checkName(GetCurrentThread(), longName);
	longName[1024] = (WCHAR)('a' + 1024 % 26);
	longName[32767] = 0;
	setName(GetCurrentThread(), longName, success);
	checkName(GetCurrentThread(), longName);
	longName[32767] = (WCHAR)('a' + 32767 % 26);
	setName(GetCurrentThread(), longName, 0xd000000d);
	longName[32767] = 0;
	checkName(GetCurrentThread(), longName);
	free(longName);
	const HANDLE invalid[] = {NULL, (HANDLE)(uintptr_t)0x1234};
	for (unsigned index = 0; index < 2; ++index) {
		setName(invalid[index], L"test", invalidHandle);
		getName(invalid[index], NULL, invalidHandle);
	}
	HANDLE gate = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(gate != NULL);
	const HANDLE nonThread[] = {gate, GetCurrentProcess()};
	for (unsigned index = 0; index < 2; ++index) {
		setName(nonThread[index], L"test", wrongType);
		getName(nonThread[index], NULL, wrongType);
	}
	DWORD id = 0;
	HANDLE thread = CreateThread(NULL, 0, worker, gate, 0, &id);
	TEST_CHECK(thread != NULL);
	setName(thread, unicode, success);
	checkName(thread, unicode);
	const DWORD rights[] = {0, THREAD_QUERY_LIMITED_INFORMATION, THREAD_SET_LIMITED_INFORMATION,
							THREAD_QUERY_INFORMATION, THREAD_SET_INFORMATION};
	for (unsigned index = 0; index < sizeof(rights) / sizeof(rights[0]); ++index) {
		HANDLE opened = NULL;
		if (rights[index])
			opened = OpenThread(rights[index], FALSE, id);
		else
			TEST_CHECK(DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &opened, 0, FALSE, 0));
		TEST_CHECK(opened != NULL);
		const BOOL canSet = (rights[index] & (THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION)) != 0;
		const BOOL canQuery = (rights[index] & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION)) != 0;
		setName(opened, unicode, canSet ? success : accessDenied);
		if (canQuery)
			checkName(opened, unicode);
		else
			getName(opened, NULL, accessDenied);
		TEST_CHECK(CloseHandle(opened));
	}
	HANDLE duplicate = NULL;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	TEST_CHECK(CloseHandle(thread));
	setName(duplicate, L"replacement", success);
	checkName(duplicate, L"replacement");
	TEST_CHECK(SetEvent(gate));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(duplicate, 5000));
	checkName(duplicate, L"");
	setName(duplicate, L"after-exit", success);
	checkName(duplicate, L"after-exit");
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(CloseHandle(gate));
	// Remote proxy state is not shared with the child runtime in the local scope.
	TEST_CHECK(checkProxy(argc == 2));
	puts("Thread description fixture complete");
	return 0;
}
