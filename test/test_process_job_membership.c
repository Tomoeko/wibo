#include <windows.h>

#include "test_assert.h"

static void expect_failure(HANDLE process, HANDLE job, DWORD error) {
	BOOL member = 42;
	TEST_CHECK(!IsProcessInJob(process, job, &member));
	TEST_CHECK_EQ(error, GetLastError());
	TEST_CHECK_EQ(42, member);
}

int main(void) {
	struct {
		BOOL member;
		DWORD guard;
	} result = {42, 0x11223344};
	SetLastError(0x20001234);
	TEST_CHECK(IsProcessInJob(GetCurrentProcess(), NULL, &result.member));
	TEST_CHECK_EQ(0x20001234, GetLastError());
	TEST_CHECK(result.member == TRUE || result.member == FALSE);
	if (getenv("WIBO_EXPECT_NO_JOB"))
		TEST_CHECK_EQ(FALSE, result.member);
	const BOOL membership = result.member;
	TEST_CHECK_EQ(0x11223344, result.guard);
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(process != NULL);
	result.member = 42;
	TEST_CHECK(IsProcessInJob(process, NULL, &result.member));
	TEST_CHECK_EQ(membership, result.member);
	TEST_CHECK(CloseHandle(process));
	HANDLE denied;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &denied, SYNCHRONIZE, FALSE, 0));
	expect_failure(denied, NULL, ERROR_ACCESS_DENIED);
	TEST_CHECK(CloseHandle(denied));
	expect_failure((HANDLE)(ULONG_PTR)0x1234, NULL, ERROR_INVALID_HANDLE);
	expect_failure(GetCurrentProcess(), (HANDLE)(ULONG_PTR)0x1234, ERROR_INVALID_HANDLE);
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	expect_failure(event, NULL, ERROR_INVALID_HANDLE);
	expect_failure(GetCurrentProcess(), event, ERROR_INVALID_HANDLE);
	TEST_CHECK(CloseHandle(event));
	return 0;
}
