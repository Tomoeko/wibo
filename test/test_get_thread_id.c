#include <windows.h>

#include "test_assert.h"

static DWORD WINAPI thread_entry(void *context) {
	(void)context;
	return 0;
}

int main(void) {
	TEST_CHECK(GetThreadId(GetCurrentThread()) == GetCurrentThreadId());

	DWORD createdId = 0;
	HANDLE thread = CreateThread(NULL, 0, thread_entry, NULL, CREATE_SUSPENDED, &createdId);
	TEST_CHECK(thread != NULL);
	TEST_CHECK(GetThreadId(thread) == createdId);
	TEST_CHECK(ResumeThread(thread) != (DWORD)-1);
	TEST_CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
	TEST_CHECK(CloseHandle(thread));
	return 0;
}
