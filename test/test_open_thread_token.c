#include <windows.h>

#include "test_assert.h"

static void expect_failure(HANDLE thread, DWORD access, BOOL asSelf, DWORD error) {
	HANDLE token = (HANDLE)(ULONG_PTR)0x12345678;
	SetLastError(0x20001234);
	TEST_CHECK(!OpenThreadToken(thread, access, asSelf, &token));
	TEST_CHECK_EQ(error, GetLastError());
	TEST_CHECK(token == NULL);
}

static DWORD WINAPI worker(void *parameter) {
	expect_failure(GetCurrentThread(), TOKEN_QUERY, FALSE, ERROR_NO_TOKEN);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject((HANDLE)parameter, 5000));
	return 0;
}

int main(void) {
	const DWORD access[] = {0, TOKEN_QUERY, TOKEN_ALL_ACCESS};
	for (unsigned index = 0; index < sizeof(access) / sizeof(access[0]); ++index) {
		expect_failure(GetCurrentThread(), access[index], FALSE, ERROR_NO_TOKEN);
		expect_failure(GetCurrentThread(), access[index], TRUE, ERROR_NO_TOKEN);
	}
	const DWORD rights[] = {0, SYNCHRONIZE, THREAD_QUERY_INFORMATION, THREAD_QUERY_LIMITED_INFORMATION};
	for (unsigned index = 0; index < sizeof(rights) / sizeof(rights[0]); ++index) {
		HANDLE copy;
		TEST_CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &copy, rights[index],
								   FALSE, 0));
		expect_failure(copy, TOKEN_QUERY, FALSE, ERROR_NO_TOKEN);
		TEST_CHECK(CloseHandle(copy));
		expect_failure(copy, TOKEN_QUERY, TRUE, ERROR_INVALID_HANDLE);
	}
	expect_failure((HANDLE)(ULONG_PTR)0x1234, TOKEN_QUERY, FALSE, ERROR_INVALID_HANDLE);
	HANDLE gate = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(gate != NULL);
	expect_failure(gate, TOKEN_QUERY, FALSE, ERROR_INVALID_HANDLE);
	HANDLE thread = CreateThread(NULL, 0, worker, gate, 0, NULL);
	TEST_CHECK(thread != NULL);
	expect_failure(thread, TOKEN_QUERY, TRUE, ERROR_NO_TOKEN);
	TEST_CHECK(SetEvent(gate));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	TEST_CHECK(CloseHandle(gate));
	return 0;
}
