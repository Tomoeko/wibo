#define _WIN32_WINNT 0x0600
#include <windows.h>

#include "test_assert.h"

struct wait_case {
	HANDLE entered;
	HANDLE release;
	HANDLE finished;
	volatile LONG count;
	volatile LONG timed_out;
};

static VOID CALLBACK wait_callback(PVOID context, BOOLEAN timed_out) {
	struct wait_case *state = (struct wait_case *)context;
	InterlockedIncrement(&state->count);
	InterlockedExchange(&state->timed_out, timed_out);
	SetEvent(state->entered);
	if (state->release)
		WaitForSingleObject(state->release, 5000);
	SetEvent(state->finished);
}

static void test_signaled_once(void) {
	struct wait_case state = {0};
	HANDLE target = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE registration = NULL;
	state.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(target && state.entered && state.finished);
	TEST_CHECK(RegisterWaitForSingleObject(&registration, target, wait_callback, &state, INFINITE, WT_EXECUTEONLYONCE));
	TEST_CHECK(registration != NULL);
	TEST_CHECK(SetEvent(target));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.finished, 5000));
	TEST_CHECK_EQ(1, state.count);
	TEST_CHECK_EQ(FALSE, state.timed_out);
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(target, 0));
	TEST_CHECK(UnregisterWaitEx(registration, INVALID_HANDLE_VALUE));
	TEST_CHECK(!CloseHandle(registration));
	CloseHandle(state.finished);
	CloseHandle(state.entered);
	CloseHandle(target);
}

static void test_timeout_once(void) {
	struct wait_case state = {0};
	HANDLE target = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE registration = NULL;
	state.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(target && state.entered && state.finished);
	TEST_CHECK(RegisterWaitForSingleObject(&registration, target, wait_callback, &state, 30, WT_EXECUTEONLYONCE));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.finished, 5000));
	TEST_CHECK_EQ(1, state.count);
	TEST_CHECK_EQ(TRUE, state.timed_out);
	TEST_CHECK(UnregisterWaitEx(registration, INVALID_HANDLE_VALUE));
	CloseHandle(state.finished);
	CloseHandle(state.entered);
	CloseHandle(target);
}

static void test_completion_event(void) {
	struct wait_case state = {0};
	HANDLE target = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE completion = CreateEventW(NULL, TRUE, FALSE, NULL);
	HANDLE registration = NULL;
	state.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(target && completion && state.entered && state.release && state.finished);
	TEST_CHECK(RegisterWaitForSingleObject(&registration, target, wait_callback, &state, INFINITE, WT_EXECUTEONLYONCE));
	TEST_CHECK(SetEvent(target));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.entered, 5000));
	{
		BOOL result = UnregisterWaitEx(registration, completion);
		TEST_CHECK(result || GetLastError() == ERROR_IO_PENDING);
	}
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(completion, 0));
	TEST_CHECK(SetEvent(state.release));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(completion, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.finished, 5000));
	TEST_CHECK_EQ(1, state.count);
	CloseHandle(state.finished);
	CloseHandle(state.release);
	CloseHandle(state.entered);
	CloseHandle(completion);
	CloseHandle(target);
}

static void test_cancel_before_signal(void) {
	struct wait_case state = {0};
	HANDLE target = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE registration = NULL;
	state.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(target && state.entered && state.finished);
	TEST_CHECK(RegisterWaitForSingleObject(&registration, target, wait_callback, &state, INFINITE, WT_EXECUTEONLYONCE));
	TEST_CHECK(UnregisterWaitEx(registration, INVALID_HANDLE_VALUE));
	TEST_CHECK(SetEvent(target));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(state.entered, 50));
	TEST_CHECK_EQ(0, state.count);
	CloseHandle(state.finished);
	CloseHandle(state.entered);
	CloseHandle(target);
}

static void test_unregister_pending_callback(void) {
	struct wait_case state = {0};
	HANDLE target = CreateEventW(NULL, FALSE, FALSE, NULL);
	HANDLE registration = NULL;
	state.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.release = CreateEventW(NULL, TRUE, FALSE, NULL);
	state.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(target && state.entered && state.release && state.finished);
	TEST_CHECK(RegisterWaitForSingleObject(&registration, target, wait_callback, &state, INFINITE, WT_EXECUTEONLYONCE));
	TEST_CHECK(SetEvent(target));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.entered, 5000));
	TEST_CHECK(!UnregisterWait(registration));
	TEST_CHECK_EQ(ERROR_IO_PENDING, GetLastError());
	TEST_CHECK(SetEvent(state.release));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state.finished, 5000));
	CloseHandle(state.finished);
	CloseHandle(state.release);
	CloseHandle(state.entered);
	CloseHandle(target);
}

int main(void) {
	test_signaled_once();
	test_timeout_once();
	test_completion_event();
	test_cancel_before_signal();
	test_unregister_pending_callback();
	return 0;
}
