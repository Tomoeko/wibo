#include <windows.h>
#include <stdint.h>
#include "test_assert.h"

#ifdef _WIN64
#error This fixture exercises the x86 registration chain.
#endif

/* Raw exception dispositions differ from __except filter return values. */
enum { ContinueExecution = 0, ContinueSearch = 1 };
enum { TestException = 0xe0420321u, NestedException = 0xe0420322u };
typedef struct Registration {
	struct Registration *next;
	void *handler;
	unsigned index;
} Registration;
typedef void (WINAPI *RaiseFn)(DWORD, DWORD, DWORD, const ULONG_PTR *);

static RaiseFn raise_fn;
static unsigned visits[8], visit_count, nested_calls;
static int nesting;
static int reject_continuation;

static Registration *get_chain(void) {
	Registration *head;
	__asm__ volatile("movl %%fs:0, %0" : "=r"(head));
	return head;
}

static void set_chain(Registration *head) {
	__asm__ volatile("movl %0, %%fs:0" : : "r"(head) : "memory");
}

static DWORD WINAPI check_thread_chain(LPVOID unused) {
	(void)unused;
	TEST_CHECK(get_chain() == (Registration *)(uintptr_t)0xffffffffu);
	return 0;
}

static LONG __cdecl handler(EXCEPTION_RECORD *record, Registration *frame,
							CONTEXT *context, Registration **dispatcher) {
	(void)dispatcher;
	TEST_CHECK(context != NULL && record != NULL);
	TEST_CHECK_EQ(CONTEXT_i386, context->ContextFlags & CONTEXT_i386);
	TEST_CHECK(context->Eip != 0 && context->Esp != 0);
	TEST_CHECK(record->ExceptionAddress != NULL);
	TEST_CHECK(GetCurrentProcessId() != 0);
	if (record->ExceptionCode == EXCEPTION_NONCONTINUABLE_EXCEPTION) {
		TEST_CHECK(reject_continuation);
		TEST_CHECK_EQ(1, record->ExceptionFlags & EXCEPTION_NONCONTINUABLE);
		puts("observed noncontinuable continuation rejection");
		fflush(stdout);
		return ContinueSearch;
	}
	if (record->ExceptionCode == NestedException) {
		++nested_calls;
		TEST_CHECK_EQ(frame->index == 0, (record->ExceptionFlags & EXCEPTION_NESTED_CALL) != 0);
		return frame->index == 1 ? ContinueExecution : ContinueSearch;
	}
	TEST_CHECK_U64_EQ(TestException, record->ExceptionCode);
	TEST_CHECK(visit_count < sizeof(visits) / sizeof(visits[0]));
	visits[visit_count++] = frame->index;
	if (reject_continuation) {
		TEST_CHECK_EQ(EXCEPTION_NONCONTINUABLE, record->ExceptionFlags);
		return ContinueExecution;
	}
	TEST_CHECK_EQ(0, record->ExceptionFlags);
	TEST_CHECK_EQ(2, record->NumberParameters);
	TEST_CHECK_U64_EQ(0x10203040u, record->ExceptionInformation[0]);
	TEST_CHECK_U64_EQ(0x50607080u, record->ExceptionInformation[1]);
	if (frame->index == 0) {
		if (nesting) {
			nesting = 0;
			raise_fn(NestedException, 0, 0, NULL);
		}
		return ContinueSearch;
	}
	context->Eax = 0x51525354u;
	return ContinueExecution;
}

static DWORD raise_and_read_eax(RaiseFn function) {
	const ULONG_PTR arguments[2] = {0x10203040u, 0x50607080u};
	DWORD result;
	__asm__ volatile(
		"pushl %[arguments]\n\t"
		"pushl $2\n\t"
		"pushl $0\n\t"
		"pushl $0xe0420321\n\t"
		"call *%[function]\n\t"
		"movl %%eax, %[result]"
		: [result] "=r"(result)
		: [arguments] "r"(arguments), [function] "r"(function)
		: "eax", "ecx", "edx", "memory", "cc");
	return result;
}

int main(int argc, char **argv) {
	Registration frames[2];
	Registration *original = get_chain();
	TEST_CHECK(original == (Registration *)(uintptr_t)0xffffffffu);
	TEST_CHECK(sizeof(CONTEXT) == 716);
	TEST_CHECK(sizeof(EXCEPTION_RECORD) == 80);
	raise_fn = (RaiseFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "RaiseException");
	TEST_CHECK(raise_fn != NULL);
	frames[0].next = &frames[1]; frames[0].handler = handler; frames[0].index = 0;
	frames[1].next = original; frames[1].handler = handler; frames[1].index = 1;
	set_chain(&frames[0]);
	if (argc == 2 && strcmp(argv[1], "noncontinuable") == 0) {
		reject_continuation = 1;
		raise_fn(TestException, EXCEPTION_NONCONTINUABLE, 0, NULL);
		TEST_FAIL("noncontinuable exception resumed");
	}
	TEST_CHECK_U64_EQ(0x51525354u, raise_and_read_eax(raise_fn));
	TEST_CHECK_EQ(2, visit_count);
	TEST_CHECK_EQ(0, visits[0]); TEST_CHECK_EQ(1, visits[1]);
	TEST_CHECK(get_chain() == &frames[0]);
	TEST_CHECK(GetCurrentProcessId() != 0);
	visit_count = 0;
	nesting = 1;
	TEST_CHECK_U64_EQ(0x51525354u, raise_and_read_eax(raise_fn));
	TEST_CHECK_EQ(2, visit_count);
	TEST_CHECK_EQ(2, nested_calls);
	TEST_CHECK(get_chain() == &frames[0]);
	set_chain(original);
	TEST_CHECK(GetCurrentProcessId() != 0);
	{
		HANDLE thread = CreateThread(NULL, 0, check_thread_chain, NULL, 0, NULL);
		DWORD status = 1;
		TEST_CHECK(thread != NULL);
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
		TEST_CHECK(GetExitCodeThread(thread, &status));
		TEST_CHECK_EQ(0, status);
		TEST_CHECK(CloseHandle(thread));
	}
	puts("x86 software exception checks passed");
	return 0;
}
