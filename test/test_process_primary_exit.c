#define _WIN32_WINNT 0x0600
#include <windows.h>

#include <stdio.h>
#include <string.h>

static const char releaseVariable[] = "WIBO_FIXTURE_PRIMARY_RELEASE";
static char childReleasePath[MAX_PATH];
static HANDLE childReady;

static DWORD WINAPI remainingWorker(void *unused) {
	(void)unused;
	if (!SetEvent(childReady))
		ExitProcess(91);
	DWORD start = GetTickCount();
	for (;;) {
		HANDLE file = CreateFileA(childReleasePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
								  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			ExitProcess(92);
		DWORD value = 0, read = 0;
		BOOL result = ReadFile(file, &value, sizeof(value), &read, NULL);
		BOOL closed = CloseHandle(file);
		if (!result || !closed)
			ExitProcess(93);
		if (read == sizeof(value))
			ExitProcess(value == 0x12345678 ? 43 : 94);
		if (read || GetTickCount() - start >= 10000)
			ExitProcess(95);
		Sleep(10);
	}
}

static int childMain(void) {
	DWORD length = GetEnvironmentVariableA(releaseVariable, childReleasePath, sizeof(childReleasePath));
	if (!length || length >= sizeof(childReleasePath))
		return 96;
	childReady = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (!childReady)
		return 97;
	HANDLE worker = CreateThread(NULL, 0, remainingWorker, NULL, 0, NULL);
	if (!worker || WaitForSingleObject(childReady, 5000) != WAIT_OBJECT_0)
		ExitProcess(98);
	BOOL workerClosed = CloseHandle(worker);
	BOOL readyClosed = CloseHandle(childReady);
	if (!workerClosed || !readyClosed)
		ExitProcess(99);
	ExitThread(42);
	return 100;
}

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE value = *handle;
	*handle = NULL;
	return CloseHandle(value) != FALSE;
}

static int releaseWorker(const char *path) {
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	DWORD value = 0x12345678, written = 0;
	BOOL result = WriteFile(file, &value, sizeof(value), &written, NULL);
	BOOL closed = CloseHandle(file);
	return result && written == sizeof(value) && closed;
}

static int parentMain(void) {
	int result = 1;
	char temporary[MAX_PATH], releasePath[MAX_PATH] = {0};
	char image[32768], command[32768 + 16];
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION child = {0};
	HANDLE threadAlias = NULL;
	DWORD threadCode = 0, processCode = 0;
	startup.cb = sizeof(startup);
	DWORD length = GetTempPathA(sizeof(temporary), temporary);
	if (!length || length >= sizeof(temporary) || !GetTempFileNameA(temporary, "pex", 0, releasePath) ||
		!SetEnvironmentVariableA(releaseVariable, releasePath))
		goto cleanup;
	length = GetModuleFileNameA(NULL, image, sizeof(image));
	if (!length || length >= sizeof(image))
		goto cleanup;
	int formatted = snprintf(command, sizeof(command), "\"%s\" child", image);
	if (formatted <= 0 || (size_t)formatted >= sizeof(command) ||
		!CreateProcessA(image, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child))
		goto cleanup;
	if (!child.hProcess || !child.hThread || child.hProcess == child.hThread || !child.dwThreadId ||
		GetThreadId(child.hThread) != child.dwThreadId ||
		!DuplicateHandle(GetCurrentProcess(), child.hThread, GetCurrentProcess(), &threadAlias, 0, FALSE,
						 DUPLICATE_SAME_ACCESS) ||
		!closeOwned(&child.hThread))
		goto cleanup;
	DWORD threadWait = WaitForSingleObject(threadAlias, 5000);
	DWORD processWait = WaitForSingleObject(child.hProcess, 0);
	BOOL threadQueried = GetExitCodeThread(threadAlias, &threadCode);
	BOOL processQueried = GetExitCodeProcess(child.hProcess, &processCode);
	printf("primary wait=%lu code=%lu process-wait=%lu code=%lu\n", (unsigned long)threadWait,
		   (unsigned long)threadCode, (unsigned long)processWait, (unsigned long)processCode);
	if (threadWait != WAIT_OBJECT_0 || !threadQueried || threadCode != 42 || processWait != WAIT_TIMEOUT ||
		!processQueried || processCode != STILL_ACTIVE || GetThreadId(threadAlias) != child.dwThreadId)
		goto cleanup;
	if (!releaseWorker(releasePath) || WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0 ||
		!GetExitCodeProcess(child.hProcess, &processCode) || processCode != 43 ||
		WaitForSingleObject(threadAlias, 0) != WAIT_OBJECT_0 || !GetExitCodeThread(threadAlias, &threadCode) ||
		threadCode != 42)
		goto cleanup;
	printf("completed primary=%lu process=%lu\n", (unsigned long)threadCode, (unsigned long)processCode);
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "primary-exit failed error=%lu pid=%lu\n", (unsigned long)GetLastError(),
				(unsigned long)child.dwProcessId);
	if (child.hProcess && WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(child.hProcess, 101);
		DWORD reaped = WaitForSingleObject(child.hProcess, 5000);
		fprintf(stderr, "cleanup terminated=%u wait=%lu\n", (unsigned)terminated, (unsigned long)reaped);
		if (!terminated || reaped != WAIT_OBJECT_0)
			result = 1;
	}
	int aliasClosed = closeOwned(&threadAlias);
	int threadClosed = closeOwned(&child.hThread);
	int processClosed = closeOwned(&child.hProcess);
	int environmentCleared = SetEnvironmentVariableA(releaseVariable, NULL) != FALSE;
	int fileDeleted = !releasePath[0] || DeleteFileA(releasePath);
	return result || !aliasClosed || !threadClosed || !processClosed || !environmentCleared || !fileDeleted;
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "child") == 0)
		return childMain();
	if (argc != 1)
		return 2;
	return parentMain();
}
