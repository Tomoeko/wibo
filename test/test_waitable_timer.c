#include "test_assert.h"
#include <windows.h>

typedef struct {
	HANDLE timer;
	HANDLE stop;
	volatile LONG consumed;
} TimerRace;

static DWORD WINAPI timer_worker(void *parameter) {
	TimerRace *race = (TimerRace *)parameter;
	HANDLE handles[2] = {race->timer, race->stop};
	for (;;) {
		DWORD result = WaitForMultipleObjects(2, handles, FALSE, 5000);
		if (result == WAIT_OBJECT_0 + 1)
			return 0;
		TEST_CHECK_EQ(WAIT_OBJECT_0, result);
		InterlockedIncrement(&race->consumed);
	}
}

static DWORD WINAPI wait_all_worker(void *parameter) {
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(2, (HANDLE *)parameter, TRUE, 5000));
	return 0;
}

static void check_wait_state(void) {
	HANDLE timers[2] = {CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS),
						CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS)};
	TEST_CHECK(timers[0] && timers[1]);
	LARGE_INTEGER due;
	due.QuadPart = 0;
	TEST_CHECK(SetWaitableTimer(timers[0], &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(timers[0], 5000));
	HANDLE waiter = CreateThread(NULL, 0, wait_all_worker, timers, 0, NULL);
	TEST_CHECK(waiter != NULL);
	Sleep(30);
	due.QuadPart = -2000000;
	TEST_CHECK(SetWaitableTimer(timers[0], &due, 0, NULL, NULL, FALSE));
	due.QuadPart = 0;
	TEST_CHECK(SetWaitableTimer(timers[1], &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(waiter, 50));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(waiter, 5000));
	TEST_CHECK(CloseHandle(waiter));
	TEST_CHECK(CloseHandle(timers[0]));
	TimerRace race = {timers[1], CreateEventW(NULL, TRUE, FALSE, NULL), 0};
	TEST_CHECK(race.stop != NULL);
	HANDLE workers[8];
	for (int i = 0; i < 8; ++i) {
		workers[i] = CreateThread(NULL, 0, timer_worker, &race, 0, NULL);
		TEST_CHECK(workers[i] != NULL);
	}
	Sleep(30);
	due.QuadPart = -100000;
	TEST_CHECK(SetWaitableTimer(race.timer, &due, 0, NULL, NULL, FALSE));
	Sleep(60);
	TEST_CHECK(SetEvent(race.stop));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(8, workers, TRUE, 5000));
	TEST_CHECK_EQ(1, race.consumed);
	for (int i = 0; i < 8; ++i)
		TEST_CHECK(CloseHandle(workers[i]));
	TEST_CHECK(CloseHandle(race.stop));
	TEST_CHECK(CloseHandle(race.timer));
}

static void check_legacy_creation(void) {
	HANDLE timer = CreateWaitableTimerA(NULL, 7, "wibo.fixture.timer.ansi");
	TEST_CHECK(timer != NULL);
	HANDLE duplicate = CreateWaitableTimerExW(NULL, L"wibo.fixture.timer.ansi", 0, TIMER_ALL_ACCESS);
	TEST_CHECK(duplicate != NULL);
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	DWORD flags = 0;
	TEST_CHECK(GetHandleInformation(timer, &flags));
	TEST_CHECK_EQ(0, flags);
	LARGE_INTEGER due;
	due.QuadPart = 0;
	TEST_CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(timer, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(duplicate, 0));
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(CloseHandle(timer));
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
	timer = CreateWaitableTimerA(&attributes, FALSE, NULL);
	TEST_CHECK(timer != NULL);
	TEST_CHECK(GetHandleInformation(timer, &flags));
	TEST_CHECK_EQ(HANDLE_FLAG_INHERIT, flags);
	TEST_CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(timer, 5000));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(timer, 0));
	TEST_CHECK(CloseHandle(timer));
}

int main(void) {
	check_legacy_creation();
	LARGE_INTEGER due;
	due.QuadPart = -200000;
	HANDLE manual =
		CreateWaitableTimerExW(NULL, L"wibo.fixture.timer", CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
	TEST_CHECK(manual != NULL);
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(manual, 0));
	HANDLE duplicate = CreateWaitableTimerExW(NULL, L"wibo.fixture.timer", 0, TIMER_ALL_ACCESS);
	TEST_CHECK(duplicate != NULL);
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	TEST_CHECK(CreateEventW(NULL, FALSE, FALSE, L"wibo.fixture.timer") == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		TEST_CHECK(!SetWaitableTimer(manual, &due, -1, NULL, NULL, FALSE));
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	}
	TEST_CHECK(SetWaitableTimer(manual, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(manual, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(duplicate, 0));
	TEST_CHECK(CancelWaitableTimer(manual));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(manual, 0));
	due.QuadPart = -500000;
	TEST_CHECK(SetWaitableTimer(manual, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(duplicate, 0));
	TEST_CHECK(CancelWaitableTimer(manual));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(manual, 70));
	HANDLE automatic = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	TEST_CHECK(automatic != NULL);
	due.QuadPart = -100000;
	TEST_CHECK(SetWaitableTimer(automatic, &due, 30, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(automatic, 5000));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(automatic, 0));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(automatic, 5000));
	TEST_CHECK(CancelWaitableTimer(automatic));
	HANDLE handles[2] = {manual, automatic};
	TEST_CHECK(SetWaitableTimer(manual, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK(SetWaitableTimer(automatic, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(2, handles, TRUE, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(manual, 0));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(automatic, 0));
	HANDLE restricted = CreateWaitableTimerExW(NULL, NULL, 0, SYNCHRONIZE);
	TEST_CHECK(restricted != NULL);
	TEST_CHECK(!SetWaitableTimer(restricted, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(!CancelWaitableTimer(restricted));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(restricted));
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(CloseHandle(manual));
	TEST_CHECK(CloseHandle(automatic));
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	due.QuadPart = (LONGLONG)(((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) + 200000;
	HANDLE absolute = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	TEST_CHECK(absolute != NULL);
	TEST_CHECK(SetWaitableTimer(absolute, &due, 0, NULL, NULL, FALSE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(absolute, 5000));
	due.QuadPart = -10000000;
	HANDLE armed = CreateWaitableTimerExW(NULL, L"wibo.fixture.timer.closed", 0, TIMER_ALL_ACCESS);
	TEST_CHECK(armed != NULL);
	TEST_CHECK(SetWaitableTimer(armed, &due, 1000, NULL, NULL, FALSE));
	TEST_CHECK(CloseHandle(armed));
	HANDLE reused = CreateWaitableTimerExW(NULL, L"wibo.fixture.timer.closed", 0, TIMER_ALL_ACCESS);
	TEST_CHECK(reused != NULL);
	TEST_CHECK_EQ(ERROR_SUCCESS, GetLastError());
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(reused, 0));
	TEST_CHECK(CloseHandle(reused));
	TEST_CHECK(CloseHandle(absolute));
	check_wait_state();
	return 0;
}
