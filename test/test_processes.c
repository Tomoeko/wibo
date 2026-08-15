#include "test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static DWORD parse_exit_code(const char *value) {
	TEST_CHECK(value != NULL);
	DWORD result = 0;
	for (const char *p = value; *p; ++p) {
		TEST_CHECK(*p >= '0' && *p <= '9');
		result = result * 10u + (DWORD)(*p - '0');
	}
	return result;
}

static int child_main(int argc, char **argv) {
	TEST_CHECK(argc >= 2);
	DWORD desiredExit = 0;
	if (argc >= 3 && strcmp(argv[1], "child") == 0) {
		desiredExit = parse_exit_code(argv[2]);
	} else {
		char exitBuffer[16];
		DWORD exitLen = GetEnvironmentVariableA("WIBO_TEST_PROC_EXIT", exitBuffer, sizeof(exitBuffer));
		TEST_CHECK(exitLen > 0 && exitLen < sizeof(exitBuffer));
		desiredExit = parse_exit_code(exitBuffer);
	}

	if (argc < 4 || strcmp(argv[3], "instant") != 0) {
		Sleep(200);
	}
	return (int)desiredExit;
}

typedef struct ConcurrentSpawnContext {
	const char *modulePath;
	HANDLE startEvent;
	DWORD desiredExit;
	BOOL succeeded;
} ConcurrentSpawnContext;

static DWORD WINAPI concurrent_spawn_worker(LPVOID parameter) {
	ConcurrentSpawnContext *context = (ConcurrentSpawnContext *)parameter;
	if (WaitForSingleObject(context->startEvent, 5000) != WAIT_OBJECT_0) {
		return 1;
	}

	char commandLine[MAX_PATH + 64];
	snprintf(commandLine, sizeof(commandLine), "\"%s\" child %lu instant", context->modulePath,
			 (unsigned long)context->desiredExit);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));
	if (!CreateProcessA(context->modulePath, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		return 2;
	}

	BOOL succeeded = WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0;
	DWORD exitCode = 0;
	succeeded = succeeded && GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == context->desiredExit;
	if (pi.hThread) {
		CloseHandle(pi.hThread);
	}
	CloseHandle(pi.hProcess);
	context->succeeded = succeeded;
	return succeeded ? 0 : 3;
}

static void test_concurrent_createprocess_first_use(const char *modulePath) {
	enum { WORKER_COUNT = 8 };
	HANDLE startEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(startEvent != NULL);

	ConcurrentSpawnContext contexts[WORKER_COUNT];
	HANDLE workers[WORKER_COUNT];
	for (DWORD i = 0; i < WORKER_COUNT; ++i) {
		contexts[i].modulePath = modulePath;
		contexts[i].startEvent = startEvent;
		contexts[i].desiredExit = 40u + i;
		contexts[i].succeeded = FALSE;
		workers[i] = CreateThread(NULL, 0, concurrent_spawn_worker, &contexts[i], 0, NULL);
		TEST_CHECK(workers[i] != NULL);
	}

	TEST_CHECK(SetEvent(startEvent));
	for (DWORD i = 0; i < WORKER_COUNT; ++i) {
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(workers[i], 15000));
		DWORD workerExit = 0;
		TEST_CHECK(GetExitCodeThread(workers[i], &workerExit));
		TEST_CHECK_EQ(0, workerExit);
		TEST_CHECK(contexts[i].succeeded);
		TEST_CHECK(CloseHandle(workers[i]));
	}
	TEST_CHECK(CloseHandle(startEvent));
}

static void test_createprocess_failure(void) {
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	SetLastError(0);
	char bogusCommandLine[] = "child";
	TEST_CHECK(
		!CreateProcessA("Z:/definitely/missing.exe", bogusCommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
	DWORD error = GetLastError();
	TEST_CHECK_MSG(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "CreateProcessA missing file -> %lu",
				   (unsigned long)error);
}

static int parent_main(void) {
	test_createprocess_failure();

	char modulePath[MAX_PATH];
	DWORD pathLen = GetModuleFileNameA(NULL, modulePath, (DWORD)sizeof(modulePath));
	TEST_CHECK(pathLen > 0 && pathLen < sizeof(modulePath));
	test_concurrent_createprocess_first_use(modulePath);

	const DWORD childExitCode = 0x24u;
	char commandLine[MAX_PATH + 64];
	snprintf(commandLine, sizeof(commandLine), "\"%s\" child %lu", modulePath, (unsigned long)childExitCode);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	GetStartupInfoA(&si);
	ZeroMemory(&pi, sizeof(pi));

	TEST_CHECK(CreateProcessA(modulePath, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
	TEST_CHECK(pi.hProcess != NULL);

	HANDLE processHandle = NULL;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &processHandle, 0, FALSE,
							   DUPLICATE_SAME_ACCESS));
	TEST_CHECK(processHandle != NULL);
	TEST_CHECK(processHandle != pi.hProcess);

	TEST_CHECK(CloseHandle(pi.hProcess));
	TEST_CHECK_EQ(WAIT_FAILED, WaitForSingleObject(pi.hProcess, 0));
	pi.hProcess = NULL;

	Sleep(50);

	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(processHandle, 0));

	DWORD exitCode = 0;
	TEST_CHECK(GetExitCodeProcess(processHandle, &exitCode));
	TEST_CHECK_EQ(STILL_ACTIVE, exitCode);

	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(processHandle, 5000));

	TEST_CHECK(GetExitCodeProcess(processHandle, &exitCode));
	TEST_CHECK_EQ(childExitCode, exitCode);

	TEST_CHECK(CloseHandle(processHandle));
	if (pi.hThread) {
		TEST_CHECK(CloseHandle(pi.hThread));
	}

	return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
	char role[16];
	DWORD roleLen = GetEnvironmentVariableA("WIBO_TEST_PROC_ROLE", role, sizeof(role));
	if (roleLen > 0 && roleLen < sizeof(role) && strcmp(role, "child") == 0) {
		return child_main(argc, argv);
	}
	if (argc > 1 && strcmp(argv[1], "child") == 0) {
		return child_main(argc, argv);
	}
	return parent_main();
}
