#include "test_assert.h"
#include <stdint.h>
#include <windows.h>
volatile LONG context_ready, context_stop;
volatile WORD context_ss, context_cs;
__attribute__((naked)) static DWORD WINAPI spin(void *unused) {
	__asm__ volatile("push %r12\n"
					 "sub $32,%rsp\n"
					 "movdqu %xmm6,(%rsp)\n"
					 "stmxcsr 16(%rsp)\n"
					 "movl $0x3f80,20(%rsp)\n"
					 "ldmxcsr 20(%rsp)\n"
					 "movabs $0x1122334455667788,%r12\n"
					 "movq %r12,%xmm6\n"
					 "mov %ss,%ax\n"
					 "mov %ax,context_ss(%rip)\n"
					 "mov %cs,%ax\n"
					 "mov %ax,context_cs(%rip)\n"
					 "movl $1,context_ready(%rip)\n"
					 "1: pause\n"
					 "cmpl $0,context_stop(%rip)\n"
					 "je 1b\n"
					 "ldmxcsr 16(%rsp)\n"
					 "movdqu (%rsp),%xmm6\n"
					 "add $32,%rsp\n"
					 "pop %r12\n"
					 "xor %eax,%eax\n"
					 "ret\n");
}
int main(void) {
	DWORD id;
	HANDLE thread = CreateThread(NULL, 0, spin, NULL, 0, &id);
	TEST_CHECK(thread != NULL);
	for (unsigned i = 0; i < 200 && !InterlockedCompareExchange(&context_ready, 0, 0); ++i)
		Sleep(1);
	TEST_CHECK(context_ready);
	TEST_CHECK_EQ(0, SuspendThread(thread));
	CONTEXT context;
	memset(&context, 0x71, sizeof(context));
	context.ContextFlags = CONTEXT_FULL;
	TEST_CHECK(GetThreadContext(thread, &context));
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, context.R12);
	TEST_CHECK(context.Rip >= (ULONG_PTR)spin && context.Rip < (ULONG_PTR)spin + 160);
	TEST_CHECK(context.Rsp != 0);
	TEST_CHECK_EQ(context_ss, context.SegSs);
	TEST_CHECK_EQ(context_cs, context.SegCs);
	TEST_CHECK_EQ(0x3f80, context.MxCsr);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, context.Xmm6.Low);
	TEST_CHECK_U64_EQ(0, context.Xmm6.High);
	TEST_CHECK_U64_EQ(0x7171717171717171ULL, context.Dr0);
	CONTEXT updated;
	memset(&updated, 0, sizeof(updated));
	updated.ContextFlags = CONTEXT_INTEGER;
	TEST_CHECK(GetThreadContext(thread, &updated));
	ULONGLONG original = updated.R12;
	updated.R12 = 0x8877665544332211ULL;
	TEST_CHECK(SetThreadContext(thread, &updated));
	memset(&updated, 0, sizeof(updated));
	updated.ContextFlags = CONTEXT_INTEGER;
	TEST_CHECK(GetThreadContext(thread, &updated));
	TEST_CHECK_U64_EQ(0x8877665544332211ULL, updated.R12);
	updated.R12 = original;
	TEST_CHECK(SetThreadContext(thread, &updated));
	CONTEXT floating;
	memset(&floating, 0, sizeof(floating));
	floating.ContextFlags = CONTEXT_FLOATING_POINT;
	TEST_CHECK(GetThreadContext(thread, &floating));
	TEST_CHECK(SetThreadContext(thread, &floating));
	CONTEXT control;
	memset(&control, 0, sizeof(control));
	control.ContextFlags = CONTEXT_CONTROL;
	TEST_CHECK(GetThreadContext(thread, &control));
	TEST_CHECK(SetThreadContext(thread, &control));
	memset(&context, 0x71, sizeof(context));
	context.ContextFlags = CONTEXT_CONTROL;
	TEST_CHECK(GetThreadContext(thread, &context));
	TEST_CHECK(context.Rip >= (ULONG_PTR)spin && context.Rip < (ULONG_PTR)spin + 160);
	TEST_CHECK_U64_EQ(0x7171717171717171ULL, context.R12);
	TEST_CHECK_U64_EQ(0x7171717171717171ULL, context.Xmm6.Low);
	TEST_CHECK_EQ(0x71717171, context.MxCsr);
	HANDLE restricted = OpenThread(THREAD_QUERY_INFORMATION, FALSE, id);
	TEST_CHECK(restricted != NULL);
	TEST_CHECK(!GetThreadContext(restricted, &context));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(!SetThreadContext(restricted, &control));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(restricted));
	TEST_CHECK_EQ(1, ResumeThread(thread));
	InterlockedExchange(&context_stop, 1);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	TEST_CHECK(CloseHandle(thread));
	return 0;
}
