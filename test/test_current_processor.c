#define _WIN32_WINNT 0x0601
#include "test_assert.h"
#include <windows.h>

#include <stddef.h>

typedef DWORD(WINAPI *CurrentProcessorFn)(void);
typedef void(WINAPI *CurrentProcessorExFn)(PPROCESSOR_NUMBER);

enum { sample_count = 128, worker_count = 2 };

struct SampleState {
	DWORD last_error;
	unsigned completed;
	ULONGLONG observed;
	ULONGLONG observed_ex;
	HANDLE start;
};

struct GuardedProcessor {
	BYTE before[16];
	PROCESSOR_NUMBER processor;
	BYTE after[16];
};

_Static_assert(sizeof(PROCESSOR_NUMBER) == 4, "Processor numbers have a fixed size");
_Static_assert(offsetof(struct GuardedProcessor, processor) == 16, "The output follows its leading canary");
_Static_assert(offsetof(struct GuardedProcessor, after) == 20, "The trailing canary follows the four-byte output");

static CurrentProcessorFn current_processor;
static CurrentProcessorExFn current_processor_ex;
static SYSTEM_INFO system_info;

static void check_processor_number(DWORD processor) {
	TEST_CHECK(processor < 64);
	if (system_info.dwNumberOfProcessors < sizeof(DWORD_PTR) * 8) {
		// A saturated pointer-sized mask can omit processors in a WOW64 group.
		TEST_CHECK(processor < system_info.dwNumberOfProcessors);
		TEST_CHECK((system_info.dwActiveProcessorMask & ((DWORD_PTR)1 << processor)) != 0);
	}
}

static void collect_samples(struct SampleState *state) {
	for (unsigned index = 0; index < sample_count; ++index) {
		SetLastError(state->last_error);
		const DWORD processor = current_processor();
		TEST_CHECK_EQ(state->last_error, GetLastError());
		check_processor_number(processor);
		struct GuardedProcessor extended;
		memset(&extended, 0xa5, sizeof(extended));
		const DWORD extended_last_error = state->last_error ^ 0x00800000;
		SetLastError(extended_last_error);
		current_processor_ex(&extended.processor);
		TEST_CHECK_EQ(extended_last_error, GetLastError());
		for (unsigned canary = 0; canary < sizeof(extended.before); ++canary) {
			TEST_CHECK_EQ(0xa5, extended.before[canary]);
			TEST_CHECK_EQ(0xa5, extended.after[canary]);
		}
		TEST_CHECK_EQ(0, extended.processor.Reserved);
		check_processor_number(extended.processor.Number);
		if (system_info.dwNumberOfProcessors < sizeof(DWORD_PTR) * 8) {
			TEST_CHECK_EQ(0, extended.processor.Group);
		}
		// Each call observes its own instant; migration can change the processor between them.
		state->observed |= 1ULL << processor;
		state->observed_ex |= 1ULL << extended.processor.Number;
		++state->completed;
		if ((index + 1) % 16 == 0) {
			Sleep(0);
		}
	}
}

static DWORD WINAPI sample_worker(PVOID argument) {
	struct SampleState *state = argument;
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(state->start, 5000));
	collect_samples(state);
	return 0;
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC exported = GetProcAddress(kernel, "GetCurrentProcessorNumber");
	_Static_assert(sizeof(exported) == sizeof(current_processor), "Resolved function pointers have the same width");
	memcpy(&current_processor, &exported, sizeof(current_processor));
	TEST_CHECK(current_processor != NULL);
	exported = GetProcAddress(kernel, "GetCurrentProcessorNumberEx");
	_Static_assert(sizeof(exported) == sizeof(current_processor_ex), "Extended function pointers have the same width");
	memcpy(&current_processor_ex, &exported, sizeof(current_processor_ex));
	TEST_CHECK(current_processor_ex != NULL);
	GetSystemInfo(&system_info);
	TEST_CHECK(system_info.dwNumberOfProcessors != 0);
	TEST_CHECK(system_info.dwActiveProcessorMask != 0);

	HANDLE start = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(start != NULL);
	struct SampleState workers[worker_count];
	HANDLE threads[worker_count];
	for (unsigned index = 0; index < worker_count; ++index) {
		workers[index] = (struct SampleState){0x43210001 + index, 0, 0, 0, start};
		threads[index] = CreateThread(NULL, 0, sample_worker, &workers[index], 0, NULL);
		TEST_CHECK(threads[index] != NULL);
	}
	TEST_CHECK(SetEvent(start));
	struct SampleState main_state = {0x43210000, 0, 0, 0, NULL};
	collect_samples(&main_state);
	TEST_CHECK_EQ(sample_count, main_state.completed);
	TEST_CHECK(main_state.observed != 0);
	TEST_CHECK(main_state.observed_ex != 0);
	printf("main pairs=%u current=0x%llx extended=0x%llx\n", main_state.completed,
		   (unsigned long long)main_state.observed, (unsigned long long)main_state.observed_ex);
	for (unsigned index = 0; index < worker_count; ++index) {
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(threads[index], 5000));
		DWORD exit_code = 1;
		TEST_CHECK(GetExitCodeThread(threads[index], &exit_code));
		TEST_CHECK_EQ(0, exit_code);
		TEST_CHECK_EQ(sample_count, workers[index].completed);
		TEST_CHECK(workers[index].observed != 0);
		TEST_CHECK(workers[index].observed_ex != 0);
		printf("worker %u pairs=%u current=0x%llx extended=0x%llx\n", index, workers[index].completed,
			   (unsigned long long)workers[index].observed, (unsigned long long)workers[index].observed_ex);
		TEST_CHECK(CloseHandle(threads[index]));
	}
	TEST_CHECK(CloseHandle(start));
	return 0;
}
