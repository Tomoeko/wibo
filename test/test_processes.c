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

	if (argc >= 5 && strcmp(argv[3], "parent") == 0) {
		HANDLE parent =
			OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, parse_exit_code(argv[4]));
		TEST_CHECK(parent != NULL);
		DWORD exitCode = 0;
		TEST_CHECK(GetExitCodeProcess(parent, &exitCode));
		TEST_CHECK_EQ(STILL_ACTIVE, exitCode);
		TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(parent, 0));
		TEST_CHECK(CloseHandle(parent));
	}
	if (argc >= 5 && strcmp(argv[3], "await-file") == 0) {
		DWORD start = GetTickCount();
		while (GetFileAttributesA(argv[4]) == INVALID_FILE_ATTRIBUTES) {
			TEST_CHECK(GetTickCount() - start < 10000);
			Sleep(10);
		}
		return (int)desiredExit;
	}
	if (argc >= 6 && strcmp(argv[3], "observe") == 0) {
		HANDLE sibling = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, parse_exit_code(argv[4]));
		TEST_CHECK(sibling != NULL);
		DWORD exitCode = 0;
		TEST_CHECK(GetExitCodeProcess(sibling, &exitCode));
		TEST_CHECK_EQ(STILL_ACTIVE, exitCode);
		TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(sibling, 0));
		HANDLE ready = CreateFileA(argv[5], GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		TEST_CHECK(ready != INVALID_HANDLE_VALUE);
		TEST_CHECK(CloseHandle(ready));
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(sibling, 10000));
		char unavailable[2];
		if (GetEnvironmentVariableA("WIBO_FIXTURE_EXTERNAL_STATUS_UNAVAILABLE", unavailable, sizeof(unavailable))) {
			TEST_CHECK(!GetExitCodeProcess(sibling, &exitCode));
			TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		} else {
			TEST_CHECK(GetExitCodeProcess(sibling, &exitCode));
			TEST_CHECK_EQ(desiredExit, exitCode);
		}
		TEST_CHECK(CloseHandle(sibling));
		return 0;
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
	HANDLE current = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, TRUE, GetCurrentProcessId());
	TEST_CHECK(current != NULL);
	DWORD handleFlags = 0, currentExit = 0;
	TEST_CHECK(GetHandleInformation(current, &handleFlags));
	TEST_CHECK_EQ(HANDLE_FLAG_INHERIT, handleFlags & HANDLE_FLAG_INHERIT);
	TEST_CHECK(GetExitCodeProcess(current, &currentExit));
	TEST_CHECK_EQ(STILL_ACTIVE, currentExit);
	TEST_CHECK_EQ(WAIT_FAILED, WaitForSingleObject(current, 0));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(WAIT_FAILED, WaitForMultipleObjects(1, &current, FALSE, 0));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(!TerminateProcess(current, 99));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(current));
	current = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
	TEST_CHECK(current != NULL);
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(current, 0));
	TEST_CHECK(!GetExitCodeProcess(current, &currentExit));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK(CloseHandle(current));
	TEST_CHECK(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, 0) == NULL);
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());

	char modulePath[MAX_PATH];
	DWORD pathLen = GetModuleFileNameA(NULL, modulePath, (DWORD)sizeof(modulePath));
	TEST_CHECK(pathLen > 0 && pathLen < sizeof(modulePath));
	test_concurrent_createprocess_first_use(modulePath);

	const DWORD childExitCode = 0x24u;
	char commandLine[MAX_PATH + 64];
	snprintf(commandLine, sizeof(commandLine), "\"%s\" child %lu parent %lu", modulePath, (unsigned long)childExitCode,
			 (unsigned long)GetCurrentProcessId());

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	GetStartupInfoA(&si);
	ZeroMemory(&pi, sizeof(pi));

	TEST_CHECK(CreateProcessA(modulePath, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
	TEST_CHECK(pi.hProcess != NULL);
	HANDLE reopened = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pi.dwProcessId);
	TEST_CHECK(reopened != NULL && reopened != pi.hProcess);

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
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(reopened, 0));
	TEST_CHECK(GetExitCodeProcess(reopened, &exitCode));
	TEST_CHECK_EQ(childExitCode, exitCode);
	TEST_CHECK(CloseHandle(reopened));

	TEST_CHECK(CloseHandle(processHandle));
	if (pi.hThread) {
		TEST_CHECK(CloseHandle(pi.hThread));
	}

	char temporaryDirectory[MAX_PATH], readyPath[MAX_PATH];
	TEST_CHECK(GetTempPathA(sizeof(temporaryDirectory), temporaryDirectory));
	TEST_CHECK(GetTempFileNameA(temporaryDirectory, "prc", 0, readyPath));
	TEST_CHECK(DeleteFileA(readyPath));
	char siblingCommand[2 * MAX_PATH + 96];
	snprintf(siblingCommand, sizeof(siblingCommand), "\"%s\" child 37 await-file \"%s\"", modulePath, readyPath);
	PROCESS_INFORMATION sibling;
	ZeroMemory(&sibling, sizeof(sibling));
	TEST_CHECK(CreateProcessA(modulePath, siblingCommand, NULL, NULL, FALSE, 0, NULL, NULL, &si, &sibling));
	snprintf(siblingCommand, sizeof(siblingCommand), "\"%s\" child 37 observe %lu \"%s\"", modulePath,
			 (unsigned long)sibling.dwProcessId, readyPath);
	TEST_CHECK(CreateProcessA(modulePath, siblingCommand, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(pi.hProcess, 15000));
	TEST_CHECK(GetExitCodeProcess(pi.hProcess, &exitCode));
	TEST_CHECK_EQ(0, exitCode);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(sibling.hProcess, 5000));
	TEST_CHECK(GetExitCodeProcess(sibling.hProcess, &exitCode));
	TEST_CHECK_EQ(37, exitCode);
	TEST_CHECK(CloseHandle(pi.hThread));
	TEST_CHECK(CloseHandle(pi.hProcess));
	TEST_CHECK(CloseHandle(sibling.hThread));
	TEST_CHECK(CloseHandle(sibling.hProcess));
	TEST_CHECK(DeleteFileA(readyPath));

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
