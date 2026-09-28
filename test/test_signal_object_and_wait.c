#include "test_assert.h"

#include <windows.h>

int main(void) {
	HANDLE signaled = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE ready = CreateEventW(NULL, TRUE, TRUE, NULL);
	TEST_CHECK(signaled && ready);
	TEST_CHECK_EQ(WAIT_OBJECT_0, SignalObjectAndWait(signaled, ready, 0, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(signaled, 0));

	HANDLE semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
	TEST_CHECK(semaphore != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, SignalObjectAndWait(semaphore, ready, 0, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(semaphore, 0));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(semaphore, 0));

	HANDLE mutex = CreateMutexW(NULL, FALSE, NULL);
	TEST_CHECK(mutex != NULL);
	TEST_CHECK_EQ(WAIT_FAILED, SignalObjectAndWait(mutex, ready, 0, FALSE));
	TEST_CHECK_EQ(ERROR_NOT_OWNER, GetLastError());
	TEST_CHECK(CloseHandle(mutex));
	TEST_CHECK(CloseHandle(semaphore));
	TEST_CHECK(CloseHandle(ready));
	TEST_CHECK(CloseHandle(signaled));
	return 0;
}
