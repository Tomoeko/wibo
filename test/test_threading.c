#include <windows.h>
#include <stdlib.h>
#include "test_assert.h"

typedef struct {
    HANDLE readyEvent;
    HANDLE goEvent;
    DWORD exitCode;
} WorkerContext;

static DWORD WINAPI worker_main(LPVOID param) {
    WorkerContext *ctx = (WorkerContext *)param;
    TEST_CHECK(SetEvent(ctx->readyEvent));
    DWORD waitResult = WaitForSingleObject(ctx->goEvent, 1000);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    return ctx->exitCode;
}

static DWORD WINAPI exit_thread_worker(LPVOID param) {
    DWORD code = *(DWORD *)param;
    ExitThread(code);
    return 0; /* unreachable */
}

static DWORD WINAPI large_stack_worker(LPVOID param) {
    (void)param;
    volatile BYTE buffer[640 * 1024];
    for (SIZE_T offset = 0; offset < sizeof(buffer); offset += 4096) {
        buffer[offset] = (BYTE)(offset >> 12);
    }
    buffer[sizeof(buffer) - 1] = 0x5a;
    return buffer[sizeof(buffer) - 1] == 0x5a ? 0 : 1;
}

int main(void) {
    HANDLE readyEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    TEST_CHECK(readyEvent != NULL);

    HANDLE goEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    TEST_CHECK(goEvent != NULL);

    WorkerContext ctx;
    ctx.readyEvent = readyEvent;
    ctx.goEvent = goEvent;
    ctx.exitCode = 0x1234;

    HANDLE thread = CreateThread(NULL, 0, worker_main, &ctx, 0, NULL);
    TEST_CHECK(thread != NULL);

    DWORD waitResult = WaitForSingleObject(readyEvent, 1000);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);

    TEST_CHECK(ResetEvent(readyEvent));

    TEST_CHECK(SetEvent(goEvent));

    waitResult = WaitForSingleObject(thread, 1000);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);

    DWORD exitCode = 0;
    TEST_CHECK(GetExitCodeThread(thread, &exitCode));
    TEST_CHECK_EQ(ctx.exitCode, exitCode);

    TEST_CHECK(CloseHandle(thread));

    HANDLE autoEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    TEST_CHECK(autoEvent != NULL);
    TEST_CHECK(SetEvent(autoEvent));
    waitResult = WaitForSingleObject(autoEvent, 0);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    TEST_CHECK(SetEvent(autoEvent));
    waitResult = WaitForSingleObject(autoEvent, 0);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    TEST_CHECK(CloseHandle(autoEvent));

    HANDLE manualEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    TEST_CHECK(manualEvent != NULL);
    TEST_CHECK(SetEvent(manualEvent));
    waitResult = WaitForSingleObject(manualEvent, 0);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    TEST_CHECK(ResetEvent(manualEvent));
    TEST_CHECK(SetEvent(manualEvent));
    waitResult = WaitForSingleObject(manualEvent, 0);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    TEST_CHECK(CloseHandle(manualEvent));

    DWORD selfExitCode = 0;
    TEST_CHECK(GetExitCodeThread(GetCurrentThread(), &selfExitCode));
    TEST_CHECK_EQ(STILL_ACTIVE, selfExitCode);

    HANDLE mutex = CreateMutexA(NULL, FALSE, NULL);
    TEST_CHECK(mutex != NULL);
    waitResult = WaitForSingleObject(mutex, 0);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    TEST_CHECK(ReleaseMutex(mutex));
    TEST_CHECK(CloseHandle(mutex));

    DWORD secondExitCode = 0x55AA;
    HANDLE exitThread = CreateThread(NULL, 0, exit_thread_worker, &secondExitCode, 0, NULL);
    TEST_CHECK(exitThread != NULL);
    waitResult = WaitForSingleObject(exitThread, 1000);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    exitCode = 0;
    TEST_CHECK(GetExitCodeThread(exitThread, &exitCode));
    TEST_CHECK_EQ(secondExitCode, exitCode);
    TEST_CHECK(CloseHandle(exitThread));

    /* Windows uses the image's stack reserve when dwStackSize is zero. This
       deliberately exceeds Darwin's small translated pthread default. */
    HANDLE largeStackThread = CreateThread(NULL, 0, large_stack_worker, NULL, 0, NULL);
    TEST_CHECK(largeStackThread != NULL);
    waitResult = WaitForSingleObject(largeStackThread, 5000);
    TEST_CHECK_EQ(WAIT_OBJECT_0, waitResult);
    exitCode = 1;
    TEST_CHECK(GetExitCodeThread(largeStackThread, &exitCode));
    TEST_CHECK_EQ(0, exitCode);
    TEST_CHECK(CloseHandle(largeStackThread));

    TEST_CHECK(CloseHandle(goEvent));
    TEST_CHECK(CloseHandle(readyEvent));

    return EXIT_SUCCESS;
}
