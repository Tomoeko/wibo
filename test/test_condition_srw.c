#include "test_assert.h"
#include <windows.h>

static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE work = CONDITION_VARIABLE_INIT;
static CONDITION_VARIABLE changed = CONDITION_VARIABLE_INIT;
static LONG ready, released, completed;
static unsigned turn;

static void sleep_on(CONDITION_VARIABLE *condition, ULONG flags) {
	SetLastError(0x71);
	TEST_CHECK_MSG(SleepConditionVariableSRW(condition, &lock, 5000, flags),
				   "Condition wait failed: error=%lu ready=%ld completed=%ld turn=%u", GetLastError(), ready, completed,
				   turn);
	TEST_CHECK_EQ(0x71, GetLastError());
}

static DWORD WINAPI shared_reader(void *unused) {
	(void)unused;
	AcquireSRWLockShared(&lock);
	InterlockedIncrement(&ready);
	WakeAllConditionVariable(&changed);
	while (!released)
		sleep_on(&work, CONDITION_VARIABLE_LOCKMODE_SHARED);
	InterlockedIncrement(&completed);
	ReleaseSRWLockShared(&lock);
	return 0;
}

static DWORD WINAPI exchange(void *unused) {
	(void)unused;
	AcquireSRWLockExclusive(&lock);
	for (unsigned i = 0; i < 200; ++i) {
		while (turn != 1)
			sleep_on(&work, 0);
		turn = 0;
		WakeConditionVariable(&work);
	}
	ReleaseSRWLockExclusive(&lock);
	return 0;
}

static void test_timeout(ULONG flags) {
	if (flags)
		AcquireSRWLockShared(&lock);
	else
		AcquireSRWLockExclusive(&lock);
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK(!SleepConditionVariableSRW(&work, &lock, i ? 10 : 0, flags));
		TEST_CHECK_EQ(ERROR_TIMEOUT, GetLastError());
		TEST_CHECK(!TryAcquireSRWLockExclusive(&lock));
		TEST_CHECK_EQ(flags != 0, TryAcquireSRWLockShared(&lock) != 0);
		if (flags)
			ReleaseSRWLockShared(&lock);
	}
	if (flags)
		ReleaseSRWLockShared(&lock);
	else
		ReleaseSRWLockExclusive(&lock);
	TEST_CHECK(TryAcquireSRWLockExclusive(&lock));
	ReleaseSRWLockExclusive(&lock);
}

int main(void) {
	test_timeout(0);
	test_timeout(CONDITION_VARIABLE_LOCKMODE_SHARED);
	HANDLE readers[4];
	AcquireSRWLockExclusive(&lock);
	for (unsigned i = 0; i < 4; ++i) {
		readers[i] = CreateThread(NULL, 0, shared_reader, NULL, 0, NULL);
		TEST_CHECK(readers[i] != NULL);
	}
	while (ready != 4)
		sleep_on(&changed, 0);
	// Acquiring exclusive access here establishes that all shared sleepers
	// released their lock, including during registration before a wake.
	released = 1;
	WakeAllConditionVariable(&work);
	ReleaseSRWLockExclusive(&lock);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(4, readers, TRUE, 5000));
	TEST_CHECK_EQ(4, completed);
	for (unsigned i = 0; i < 4; ++i)
		TEST_CHECK(CloseHandle(readers[i]));
	HANDLE thread = CreateThread(NULL, 0, exchange, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	AcquireSRWLockExclusive(&lock);
	for (unsigned i = 0; i < 200; ++i) {
		turn = 1;
		WakeConditionVariable(&work);
		while (turn != 0)
			sleep_on(&work, 0);
	}
	ReleaseSRWLockExclusive(&lock);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	return 0;
}
