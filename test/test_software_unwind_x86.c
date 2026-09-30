#include <windows.h>
#include <stdint.h>
#include "test_assert.h"

#ifdef _WIN64
#error This fixture exercises the x86 registration chain.
#endif

typedef struct Registration {
	struct Registration *next;
	void *handler;
	unsigned index;
} Registration;
typedef void (WINAPI *UnwindFn)(void *, void *, EXCEPTION_RECORD *, void *);
static unsigned visits[4], visit_count;
static int exit_unwind;
static int generated_record;

static Registration *get_chain(void) {
	Registration *head;
	__asm__ volatile("movl %%fs:0, %0" : "=r"(head));
	return head;
}

static void set_chain(Registration *head) {
	__asm__ volatile("movl %0, %%fs:0" : : "r"(head) : "memory");
}

static LONG __cdecl cleanup(EXCEPTION_RECORD *record, Registration *frame,
							CONTEXT *context, Registration **dispatcher) {
	(void)dispatcher;
	TEST_CHECK(record != NULL && context != NULL);
	TEST_CHECK(record->ExceptionFlags & EXCEPTION_UNWINDING);
	TEST_CHECK_EQ(exit_unwind != 0, (record->ExceptionFlags & EXCEPTION_EXIT_UNWIND) != 0);
	TEST_CHECK_U64_EQ(generated_record ? 0xc0000027u : 0xe0420323u, record->ExceptionCode);
	TEST_CHECK(GetCurrentProcessId() != 0);
	TEST_CHECK(visit_count < sizeof(visits) / sizeof(visits[0]));
	visits[visit_count++] = frame->index;
	if (exit_unwind && frame->index == 1) {
		puts("observed exit-unwind cleanup for both installed frames");
		fflush(stdout);
	}
	return 1; /* ExceptionContinueSearch during unwind. */
}

static DWORD unwind_and_read_eax(UnwindFn function, Registration *target,
								 EXCEPTION_RECORD *record, DWORD value) {
	const uintptr_t arguments[4] = {(uintptr_t)target, 0, (uintptr_t)record, value};
	DWORD result;
	__asm__ volatile(
		"pushl 12(%[arguments])\n\t"
		"pushl 8(%[arguments])\n\t"
		"pushl 4(%[arguments])\n\t"
		"pushl 0(%[arguments])\n\t"
		"call *%[function]\n\t"
		"movl %%eax, %[result]"
		: [result] "=r"(result)
		: [arguments] "r"(arguments), [function] "r"(function)
		: "eax", "ecx", "edx", "memory", "cc");
	return result;
}

int main(int argc, char **argv) {
	Registration frames[3];
	Registration *original = get_chain();
	TEST_CHECK(original == (Registration *)(uintptr_t)0xffffffffu);
	EXCEPTION_RECORD record;
	UnwindFn unwind = (UnwindFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlUnwind");
	TEST_CHECK(unwind != NULL);
	memset(&record, 0, sizeof(record));
	record.ExceptionCode = 0xe0420323u;
	frames[0].next = &frames[1]; frames[0].handler = cleanup; frames[0].index = 0;
	frames[1].next = &frames[2]; frames[1].handler = cleanup; frames[1].index = 1;
	frames[2].next = original; frames[2].handler = cleanup; frames[2].index = 2;
	set_chain(&frames[0]);
	if (argc == 2 && strcmp(argv[1], "exit") == 0) {
		exit_unwind = 1;
		generated_record = 1;
		frames[1].next = original;
		unwind(NULL, NULL, NULL, NULL);
		TEST_FAIL("exit unwind resumed after the chain ended");
	}
	TEST_CHECK_U64_EQ(0x10293847u, unwind_and_read_eax(unwind, &frames[1], &record, 0x10293847u));
	TEST_CHECK_EQ(1, visit_count); TEST_CHECK_EQ(0, visits[0]);
	TEST_CHECK(get_chain() == &frames[1]);
	TEST_CHECK(GetCurrentProcessId() != 0);
	set_chain(original);
	visit_count = 0;
	generated_record = 1;
	set_chain(&frames[0]);
	TEST_CHECK_U64_EQ(0x11223344u, unwind_and_read_eax(unwind, &frames[2], NULL, 0x11223344u));
	TEST_CHECK_EQ(2, visit_count);
	TEST_CHECK_EQ(0, visits[0]); TEST_CHECK_EQ(1, visits[1]);
	TEST_CHECK(get_chain() == &frames[2]);
	set_chain(original);
	TEST_CHECK(GetCurrentProcessId() != 0);
	puts("x86 software unwind checks passed");
	return 0;
}
