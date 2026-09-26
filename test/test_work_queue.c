#include "test_assert.h"
#include <windows.h>
#define COUNT 32
static DWORD tlsIndex, mainThread;
static HANDLE done, gate, started, finished;
static volatile LONG completed;
typedef struct {
	ULONG_PTR token;
	volatile LONG calls;
	DWORD thread;
} Job;
static Job jobs[COUNT];
static DWORD WINAPI work(void *context) {
	Job *job = (Job *)context;
	TEST_CHECK_EQ(1, InterlockedIncrement(&job->calls));
	const ULONG_PTR address = (ULONG_PTR)context, base = (ULONG_PTR)jobs;
	TEST_CHECK(address >= base && address < base + sizeof(jobs) && (address - base) % sizeof(Job) == 0);
	TEST_CHECK_U64_EQ(((ULONG_PTR)1 << (sizeof(ULONG_PTR) * 8 - 1)) + (address - base) / sizeof(Job), job->token);
	job->thread = GetCurrentThreadId();
	TEST_CHECK(job->thread != mainThread);
	TEST_CHECK(TlsSetValue(tlsIndex, (void *)(ULONG_PTR)job->thread));
	Sleep(1);
	TEST_CHECK_U64_EQ(job->thread, (ULONG_PTR)TlsGetValue(tlsIndex));
	if (InterlockedIncrement(&completed) == COUNT)
		TEST_CHECK(SetEvent(done));
	return 0xfedcba98;
}
static DWORD WINAPI blocking(void *context) {
	TEST_CHECK(context == gate);
	TEST_CHECK(ReleaseSemaphore(started, 1, NULL));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(gate, 30000));
	TEST_CHECK(ReleaseSemaphore(finished, 1, NULL));
	return 0;
}
static DWORD WINAPI signalWork(void *context) {
	TEST_CHECK(SetEvent((HANDLE)context));
	return 0;
}
static DWORD WINAPI nested(void *context) {
	TEST_CHECK(context == NULL);
	TEST_CHECK(QueueUserWorkItem(work, &jobs[0], WT_EXECUTEDEFAULT));
	return 0;
}
int main(void) {
	mainThread = GetCurrentThreadId();
	tlsIndex = TlsAlloc();
	TEST_CHECK(tlsIndex != TLS_OUT_OF_INDEXES);
	TEST_CHECK(TlsSetValue(tlsIndex, (void *)(ULONG_PTR)0x71));
	done = CreateEventW(NULL, TRUE, FALSE, NULL);
	gate = CreateEventW(NULL, TRUE, FALSE, NULL);
	started = CreateSemaphoreW(NULL, 0, 64, NULL);
	finished = CreateSemaphoreW(NULL, 0, 64, NULL);
	TEST_CHECK(done && gate && started && finished);
	for (unsigned i = 0; i < COUNT; ++i)
		jobs[i].token = ((ULONG_PTR)1 << (sizeof(ULONG_PTR) * 8 - 1)) + i;
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	const unsigned blockers = info.dwNumberOfProcessors < 64 ? info.dwNumberOfProcessors : 64;
	for (unsigned i = 0; i < blockers; ++i)
		TEST_CHECK(QueueUserWorkItem(blocking, gate, WT_EXECUTEDEFAULT));
	for (unsigned i = 0; i < blockers; ++i)
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(started, 30000));
	TEST_CHECK(QueueUserWorkItem(signalWork, done, WT_EXECUTEDEFAULT));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(done, 5000));
	TEST_CHECK(SetEvent(gate));
	for (unsigned i = 0; i < blockers; ++i)
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(finished, 5000));
	TEST_CHECK(ResetEvent(done));
	TEST_CHECK(ResetEvent(gate));
	TEST_CHECK(QueueUserWorkItem(blocking, gate, WT_EXECUTELONGFUNCTION));
	TEST_CHECK(QueueUserWorkItem(blocking, gate, WT_EXECUTELONGFUNCTION));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(started, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(started, 5000));
	SetLastError(0x51);
	TEST_CHECK(QueueUserWorkItem(nested, NULL, WT_EXECUTEDEFAULT));
	TEST_CHECK_EQ(0x51, GetLastError());
	const ULONG flags[] = {WT_EXECUTEDEFAULT, WT_EXECUTELONGFUNCTION, WT_EXECUTEINPERSISTENTTHREAD,
						   WT_EXECUTEINIOTHREAD};
	for (unsigned i = 1; i < COUNT; ++i)
		TEST_CHECK(QueueUserWorkItem(work, &jobs[i], flags[i % 4]));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(done, 5000));
	TEST_CHECK_EQ(COUNT, completed);
	for (unsigned i = 0; i < COUNT; ++i)
		TEST_CHECK_EQ(1, jobs[i].calls);
	TEST_CHECK_U64_EQ(0x71, (ULONG_PTR)TlsGetValue(tlsIndex));
	TEST_CHECK(SetEvent(gate));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(finished, 5000));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(finished, 5000));
	TEST_CHECK(CloseHandle(done));
	TEST_CHECK(CloseHandle(gate));
	TEST_CHECK(CloseHandle(started));
	TEST_CHECK(CloseHandle(finished));
	TEST_CHECK(TlsFree(tlsIndex));
	return 0;
}
