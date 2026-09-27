#define _WIN32_WINNT 0x0601
#include "test_assert.h"
#include <windows.h>

#include <stddef.h>

static const DWORD seed = 0x13579bdf;
static const DWORD failure = (DWORD)-1;
static int unavailable;
static SYSTEM_INFO system_info;

struct GuardedProcessor {
	DWORD before;
	PROCESSOR_NUMBER processor;
	DWORD after;
};
_Static_assert(sizeof(PROCESSOR_NUMBER) == 4, "Processor numbers occupy four bytes");
_Static_assert(offsetof(struct GuardedProcessor, processor) == 4, "The processor follows the leading canary");
_Static_assert(offsetof(struct GuardedProcessor, after) == 8, "The trailing canary follows the processor");

static struct GuardedProcessor guarded(void) {
	struct GuardedProcessor output = {0x89abcdef, {0xa5a5, 0xa5, 0xa5}, 0x01234567};
	return output;
}
static void check_number(const PROCESSOR_NUMBER *processor) {
	TEST_CHECK_EQ(0, processor->Reserved);
	TEST_CHECK(processor->Number < MAXIMUM_PROCESSORS);
	if (system_info.dwNumberOfProcessors < MAXIMUM_PROCESSORS) {
		TEST_CHECK_EQ(0, processor->Group);
		TEST_CHECK(processor->Number < system_info.dwNumberOfProcessors);
	}
}
static void query(HANDLE thread, DWORD expected_error) {
	struct GuardedProcessor output = guarded(), untouched = output;
	SetLastError(seed);
	BOOL result = GetThreadIdealProcessorEx(thread, &output.processor);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(untouched.before, output.before);
	TEST_CHECK_EQ(untouched.after, output.after);
	if (unavailable) {
		TEST_CHECK(!result);
		TEST_CHECK_EQ(expected_error, error);
		TEST_CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);
	} else if (result) {
		TEST_CHECK_EQ(seed, error);
		check_number(&output.processor);
	} else {
		TEST_CHECK(error == ERROR_NOT_SUPPORTED || error == ERROR_CALL_NOT_IMPLEMENTED);
		TEST_CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);
	}
}
static void legacy(HANDLE thread, DWORD number, DWORD expected_error) {
	SetLastError(seed);
	DWORD result = SetThreadIdealProcessor(thread, number);
	DWORD error = GetLastError();
	if (unavailable || expected_error == ERROR_INVALID_PARAMETER) {
		TEST_CHECK_EQ(failure, result);
		TEST_CHECK_EQ(expected_error, error);
	} else if (result == failure) {
		TEST_CHECK(error == ERROR_NOT_SUPPORTED || error == ERROR_CALL_NOT_IMPLEMENTED);
	} else {
		TEST_CHECK(result < MAXIMUM_PROCESSORS);
		TEST_CHECK_EQ(seed, error);
	}
}
static void extended(HANDLE thread, PROCESSOR_NUMBER desired, int mode, DWORD expected_error) {
	struct GuardedProcessor output = guarded();
	if (mode == 2)
		output.processor = desired;
	const struct GuardedProcessor untouched = output;
	SetLastError(seed);
	BOOL result = SetThreadIdealProcessorEx(thread, mode == 2 ? &output.processor : &desired,
											mode == 1 ? NULL : &output.processor);
	DWORD error = GetLastError();
	TEST_CHECK_EQ(untouched.before, output.before);
	TEST_CHECK_EQ(untouched.after, output.after);
	if (unavailable) {
		TEST_CHECK(!result);
		TEST_CHECK_EQ(expected_error, error);
		TEST_CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);
	} else if (result) {
		TEST_CHECK_EQ(seed, error);
		if (mode != 1)
			check_number(&output.processor);
	} else {
		TEST_CHECK(error == ERROR_NOT_SUPPORTED || error == ERROR_CALL_NOT_IMPLEMENTED);
		TEST_CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);
	}
}
static const PROCESSOR_NUMBER first_processor = {0, 0, 0};
static DWORD WINAPI worker(void *argument) {
	HANDLE gate = argument;
	query(GetCurrentThread(), ERROR_NOT_SUPPORTED);
	legacy(GetCurrentThread(), MAXIMUM_PROCESSORS, ERROR_NOT_SUPPORTED);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(gate, 5000));
	return 0;
}
static HANDLE duplicate(HANDLE source, DWORD access, DWORD options) {
	HANDLE result = NULL;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &result, access, FALSE, options));
	TEST_CHECK(result != NULL);
	return result;
}

int main(int argc, char **argv) {
	unavailable = argc == 2 && strcmp(argv[1], "unavailable") == 0;
	TEST_CHECK(argc == 1 || unavailable);
	GetSystemInfo(&system_info);
	TEST_CHECK(system_info.dwNumberOfProcessors != 0);
	HANDLE main_thread = duplicate(GetCurrentThread(), 0, DUPLICATE_SAME_ACCESS);
	HANDLE gate = CreateEventW(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(gate != NULL);
	HANDLE created = CreateThread(NULL, 0, worker, gate, CREATE_SUSPENDED, NULL);
	TEST_CHECK(created != NULL);
	HANDLE copy = duplicate(created, 0, DUPLICATE_SAME_ACCESS);
	HANDLE query_only = duplicate(created, THREAD_QUERY_LIMITED_INFORMATION, 0);
	HANDLE set_only = duplicate(created, THREAD_SET_INFORMATION, 0);
	HANDLE limited_set = duplicate(created, THREAD_SET_LIMITED_INFORMATION, 0);
	HANDLE denied = duplicate(created, SYNCHRONIZE, 0);
	HANDLE closed = duplicate(created, 0, DUPLICATE_SAME_ACCESS);
	TEST_CHECK(CloseHandle(closed));

	const HANDLE full_access[] = {GetCurrentThread(), main_thread, created, copy};
	for (unsigned index = 0; index < sizeof(full_access) / sizeof(full_access[0]); ++index) {
		query(full_access[index], ERROR_NOT_SUPPORTED);
		legacy(full_access[index], MAXIMUM_PROCESSORS, ERROR_NOT_SUPPORTED);
		legacy(full_access[index], 0, ERROR_NOT_SUPPORTED);
		extended(full_access[index], first_processor, 0, ERROR_NOT_SUPPORTED);
		extended(full_access[index], first_processor, 1, ERROR_NOT_SUPPORTED);
		extended(full_access[index], first_processor, 2, ERROR_NOT_SUPPORTED);
	}
	legacy(main_thread, failure, ERROR_INVALID_PARAMETER);
	if (unavailable) {
		query(query_only, ERROR_NOT_SUPPORTED);
		query(set_only, ERROR_ACCESS_DENIED);
		query(limited_set, ERROR_ACCESS_DENIED);
		query(denied, ERROR_ACCESS_DENIED);
		legacy(query_only, MAXIMUM_PROCESSORS, ERROR_ACCESS_DENIED);
		legacy(limited_set, MAXIMUM_PROCESSORS, ERROR_ACCESS_DENIED);
		legacy(set_only, MAXIMUM_PROCESSORS, ERROR_NOT_SUPPORTED);
		extended(query_only, first_processor, 0, ERROR_ACCESS_DENIED);
		extended(limited_set, first_processor, 0, ERROR_ACCESS_DENIED);
		extended(set_only, first_processor, 0, ERROR_NOT_SUPPORTED);
		const HANDLE invalid[] = {NULL, GetCurrentProcess(), gate, closed};
		for (unsigned index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
			query(invalid[index], ERROR_INVALID_HANDLE);
			legacy(invalid[index], MAXIMUM_PROCESSORS, ERROR_INVALID_HANDLE);
			extended(invalid[index], first_processor, 0, ERROR_INVALID_HANDLE);
		}
		legacy(main_thread, MAXIMUM_PROCESSORS + 1, ERROR_INVALID_PARAMETER);
		if (system_info.dwNumberOfProcessors < MAXIMUM_PROCESSORS)
			legacy(main_thread, system_info.dwNumberOfProcessors, ERROR_INVALID_PARAMETER);
		PROCESSOR_NUMBER outside = {0, (BYTE)system_info.dwNumberOfProcessors, 0};
		extended(main_thread, outside, 0, ERROR_INVALID_PARAMETER);
		outside = (PROCESSOR_NUMBER){0, 255, 0};
		extended(main_thread, outside, 2, ERROR_INVALID_PARAMETER);
		outside = (PROCESSOR_NUMBER){1, 0, 0};
		extended(main_thread, outside, 0, ERROR_NOT_SUPPORTED);
		outside = (PROCESSOR_NUMBER){0, 0, 1};
		extended(main_thread, outside, 0, ERROR_NOT_SUPPORTED);
		query(GetCurrentThread(), ERROR_NOT_SUPPORTED);
		query(copy, ERROR_NOT_SUPPORTED);
	}

	TEST_CHECK_EQ(1, ResumeThread(created));
	TEST_CHECK(SetEvent(gate));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(created, 5000));
	DWORD code;
	TEST_CHECK(GetExitCodeThread(created, &code));
	TEST_CHECK_EQ(0, code);
	query(created, ERROR_NOT_SUPPORTED);
	legacy(created, MAXIMUM_PROCESSORS, ERROR_NOT_SUPPORTED);
	extended(created, first_processor, 0, ERROR_NOT_SUPPORTED);
	const HANDLE handles[] = {main_thread, created, copy, query_only, set_only, limited_set, denied, gate};
	for (unsigned index = 0; index < sizeof(handles) / sizeof(handles[0]); ++index)
		TEST_CHECK(CloseHandle(handles[index]));
	return 0;
}
