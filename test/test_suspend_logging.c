#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { kCycles = 256 };

static volatile LONG stopped, iterations, wrongIdentity;
static DWORD expectedThreadId;
static HANDLE workerReady;

static DWORD WINAPI queryIdentity(void *unused) {
	(void)unused;
	if (!SetEvent(workerReady))
		return 21;
	while (!InterlockedCompareExchange(&stopped, 0, 0)) {
		if (GetCurrentThreadId() != expectedThreadId)
			InterlockedExchange(&wrongIdentity, 1);
		InterlockedIncrement(&iterations);
	}
	return 7;
}

static int closeOwned(HANDLE *handle) {
	if (!*handle)
		return 1;
	HANDLE owned = *handle;
	*handle = NULL;
	return CloseHandle(owned) != FALSE;
}

int main(int argc, char **argv) {
	BOOL contextUnavailable = argc == 2 && !strcmp(argv[1], "--context-unavailable");
	if (argc != 1 && !contextUnavailable)
		return 2;
	HANDLE worker = NULL;
	BOOL suspended = FALSE;
	DWORD completed = 0, contextError = 0, resumeError = 0, workerExit = 0;
	DWORD contextErrorsChanged = 0, resumeErrorsChanged = 0;
	DWORD unavailableContexts = 0;
	int failure = 1;
	workerReady = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!workerReady)
		goto cleanup;
	worker = CreateThread(NULL, 0, queryIdentity, NULL, CREATE_SUSPENDED, &expectedThreadId);
	if (!worker)
		goto cleanup;
	suspended = TRUE;
	DWORD initialCount = ResumeThread(worker);
	if (initialCount != (DWORD)-1)
		suspended = FALSE;
	if (initialCount != 1 || !expectedThreadId || WaitForSingleObject(workerReady, 3000) != WAIT_OBJECT_0)
		goto cleanup;
	for (DWORD cycle = 0; cycle < kCycles; ++cycle) {
		Sleep(1);
		DWORD prior = SuspendThread(worker);
		if (prior == (DWORD)-1)
			goto cleanup;
		suspended = TRUE;
		struct {
			BYTE before[16];
			CONTEXT context;
			BYTE after[16];
		} output;
		memset(&output, 0xa5, sizeof(output));
		output.context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
		const CONTEXT before = output.context;
		SetLastError(0x4321);
		BOOL queried = GetThreadContext(worker, &output.context);
		contextError = GetLastError();
		SetLastError(0x4321);
		DWORD resumed = ResumeThread(worker);
		resumeError = GetLastError();
		if (resumed != (DWORD)-1)
			suspended = FALSE;
		if (prior || resumed != 1 || resumeError != 0x4321)
			goto cleanup;
		if (contextUnavailable) {
			if (queried || contextError != ERROR_NOT_SUPPORTED || memcmp(&output.context, &before, sizeof(before)))
				goto cleanup;
			++unavailableContexts;
		} else if (!queried || contextError != 0x4321) {
			goto cleanup;
		}
		contextErrorsChanged += contextError != 0x4321;
		resumeErrorsChanged += resumeError != 0x4321;
		for (unsigned index = 0; index < 16; ++index) {
			if (output.before[index] != 0xa5 || output.after[index] != 0xa5)
				goto cleanup;
		}
#ifdef _WIN64
		if (!contextUnavailable && (!output.context.Rip || !output.context.Rsp))
			goto cleanup;
#else
		if (!contextUnavailable && (!output.context.Eip || !output.context.Esp))
			goto cleanup;
#endif
		++completed;
	}
	failure = 0;

cleanup:
	if (suspended) {
		DWORD resumed = ResumeThread(worker);
		if (resumed == (DWORD)-1)
			failure = 1;
		else
			suspended = FALSE;
	}
	InterlockedExchange(&stopped, 1);
	DWORD wait = worker ? WaitForSingleObject(worker, 5000) : WAIT_FAILED;
	if (!worker || wait != WAIT_OBJECT_0 || !GetExitCodeThread(worker, &workerExit) || workerExit != 7)
		failure = 1;
	LONG calls = InterlockedCompareExchange(&iterations, 0, 0);
	LONG wrong = InterlockedCompareExchange(&wrongIdentity, 0, 0);
	if (completed != kCycles || calls <= 0 || wrong)
		failure = 1;
	printf("completed=%lu target=%u calls=%ld wrong_identity=%ld wait=%lu exit=%lu\n", (unsigned long)completed,
		   kCycles, (long)calls, (long)wrong, (unsigned long)wait, (unsigned long)workerExit);
	printf("context_error=%lu changed=%lu resume_error=%lu changed=%lu\n", (unsigned long)contextError,
		   (unsigned long)contextErrorsChanged, (unsigned long)resumeError, (unsigned long)resumeErrorsChanged);
	printf("context_unavailable=%lu\n", (unsigned long)unavailableContexts);
	printf("software_checks_failed=%d\n", failure);
	fflush(stdout);
	if (worker && wait != WAIT_OBJECT_0)
		ExitProcess(1);
	int workerClosed = closeOwned(&worker);
	int readyClosed = closeOwned(&workerReady);
	return failure || !workerClosed || !readyClosed;
}
