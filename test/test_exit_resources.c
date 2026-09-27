#define _WIN32_WINNT 0x0600
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "test_assert.h"

struct WorkerState {
	HANDLE ready;
	HANDLE blocked;
};

static DWORD WINAPI waitWorker(LPVOID parameter) {
	struct WorkerState *state = parameter;
	if (!SetEvent(state->ready))
		return 1;
	return WaitForSingleObject(state->blocked, INFINITE) == WAIT_OBJECT_0 ? 0 : 2;
}

static int childMain(BOOL terminate) {
	HANDLE blocked = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(blocked != NULL);
	struct WorkerState states[2];
	HANDLE workers[2];
	for (unsigned index = 0; index < 2; ++index) {
		states[index].ready = CreateEventA(NULL, TRUE, FALSE, NULL);
		states[index].blocked = blocked;
		TEST_CHECK(states[index].ready != NULL);
		workers[index] = CreateThread(NULL, 0, waitWorker, &states[index], 0, NULL);
		TEST_CHECK(workers[index] != NULL);
	}
	for (unsigned index = 0; index < 2; ++index) {
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(states[index].ready, 2000));
		TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(workers[index], 0));
	}
	HANDLE notification = CreateMemoryResourceNotification(LowMemoryResourceNotification);
	TEST_CHECK(notification != NULL);
	BOOL memoryState = 7;
	TEST_CHECK(QueryMemoryResourceNotification(notification, &memoryState));
	TEST_CHECK(memoryState == FALSE || memoryState == TRUE);
	// These live resources remain owned by the process when it terminates.
	if (terminate) {
		TerminateProcess(GetCurrentProcess(), 43);
	} else {
		ExitProcess(42);
	}
	return 1;
}

static void testChild(const char *mode, DWORD expectedCode) {
	char image[32768];
	DWORD length = GetModuleFileNameA(NULL, image, sizeof(image));
	TEST_CHECK(length > 0 && length < sizeof(image));
	char command[32768 + 32];
	int formatted = snprintf(command, sizeof(command), "\"%s\" child %s", image, mode);
	TEST_CHECK(formatted > 0 && (size_t)formatted < sizeof(command));
	STARTUPINFOA startup = {0};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {0};
	TEST_CHECK(CreateProcessA(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));

	DWORD waited = WaitForSingleObject(process.hProcess, 5000);
	DWORD exitCode = STILL_ACTIVE;
	BOOL queried = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode);
	BOOL cleaned = TRUE;
	if (waited != WAIT_OBJECT_0) {
		// Reap the owned child before reporting a failed bounded wait.
		cleaned = TerminateProcess(process.hProcess, 97);
		cleaned = WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0 && cleaned;
	}
	BOOL threadClosed = CloseHandle(process.hThread);
	BOOL processClosed = CloseHandle(process.hProcess);
	printf("mode=%s wait=%lu exit=%lu cleanup=%u\n", mode, (unsigned long)waited, (unsigned long)exitCode,
		   (unsigned)cleaned);
	TEST_CHECK(cleaned && threadClosed && processClosed);
	TEST_CHECK_EQ(WAIT_OBJECT_0, waited);
	TEST_CHECK(queried);
	TEST_CHECK_EQ(expectedCode, exitCode);
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "child") == 0) {
		TEST_CHECK(strcmp(argv[2], "exit") == 0 || strcmp(argv[2], "terminate") == 0);
		return childMain(strcmp(argv[2], "terminate") == 0);
	}
	TEST_CHECK(SetEnvironmentVariableA("WIBO_SYSTEM_PROVIDER_PERSISTENT", NULL));
	TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_MEMORY_RESOURCE_STATE_FILE", NULL));
	testChild("exit", 42);
	testChild("terminate", 43);
	return 0;
}
