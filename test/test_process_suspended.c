#define _WIN32_WINNT 0x0600
#include <windows.h>

#include <stdio.h>
#include <string.h>

static const char tlsVariable[] = "WIBO_FIXTURE_SUSPEND_TLS";
static const char entryVariable[] = "WIBO_FIXTURE_SUSPEND_ENTRY";
static const char releaseVariable[] = "WIBO_FIXTURE_SUSPEND_RELEASE";
static DWORD tlsThreadId;
static DWORD tlsError;

struct Marker {
	DWORD stage;
	DWORD threadId;
};

static int writeMarker(const char *path, DWORD stage, DWORD threadId) {
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	struct Marker marker = {stage, threadId};
	DWORD written = 0;
	BOOL result = WriteFile(file, &marker, sizeof(marker), &written, NULL);
	BOOL closed = CloseHandle(file);
	return result && written == sizeof(marker) && closed;
}

static void NTAPI markTls(PVOID module, DWORD reason, PVOID reserved) {
	(void)module;
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return;
	char path[MAX_PATH];
	DWORD length = GetEnvironmentVariableA(tlsVariable, path, sizeof(path));
	if (!length)
		return;
	if (length >= sizeof(path)) {
		tlsError = ERROR_INSUFFICIENT_BUFFER;
		return;
	}
	tlsThreadId = GetCurrentThreadId();
	if (!writeMarker(path, 1, tlsThreadId))
		tlsError = ERROR_WRITE_FAULT;
}

PIMAGE_TLS_CALLBACK suspendTlsCallback __attribute__((section(".CRT$XLB"), used)) = markTls;

static int childMain(void) {
	char entryPath[MAX_PATH], releasePath[MAX_PATH];
	DWORD entryLength = GetEnvironmentVariableA(entryVariable, entryPath, sizeof(entryPath));
	DWORD releaseLength = GetEnvironmentVariableA(releaseVariable, releasePath, sizeof(releasePath));
	DWORD threadId = GetCurrentThreadId();
	if (!entryLength || entryLength >= sizeof(entryPath) || !releaseLength || releaseLength >= sizeof(releasePath) ||
		tlsError || !tlsThreadId || threadId != tlsThreadId || !writeMarker(entryPath, 2, threadId))
		return 91;
	DWORD start = GetTickCount();
	for (;;) {
		HANDLE file = CreateFileA(releasePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
								  FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return 92;
		DWORD value = 0, read = 0;
		BOOL result = ReadFile(file, &value, sizeof(value), &read, NULL);
		BOOL closed = CloseHandle(file);
		if (!result || !closed)
			return 93;
		if (read == sizeof(value))
			return value == 0x12345678 ? 42 : 94;
		if (read || GetTickCount() - start >= 10000)
			return 95;
		Sleep(10);
	}
}

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE value = *handle;
	*handle = NULL;
	return CloseHandle(value) != FALSE;
}

static int emptyFile(const char *path) {
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	LARGE_INTEGER size;
	BOOL queried = GetFileSizeEx(file, &size);
	BOOL closed = CloseHandle(file);
	return queried && size.QuadPart == 0 && closed;
}

static int readMarker(const char *path, struct Marker *marker) {
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	unsigned char bytes[sizeof(*marker) + 1];
	DWORD read = 0;
	BOOL result = ReadFile(file, bytes, sizeof(bytes), &read, NULL);
	BOOL closed = CloseHandle(file);
	if (!result || !closed || read != sizeof(*marker))
		return 0;
	memcpy(marker, bytes, sizeof(*marker));
	return 1;
}

static int waitForMarker(const char *path, struct Marker *marker) {
	DWORD start = GetTickCount();
	do {
		if (readMarker(path, marker))
			return 1;
		Sleep(10);
	} while (GetTickCount() - start < 5000);
	return 0;
}

static int releaseChild(const char *path) {
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	DWORD value = 0x12345678, written = 0;
	BOOL result = WriteFile(file, &value, sizeof(value), &written, NULL);
	BOOL closed = CloseHandle(file);
	return result && written == sizeof(value) && closed;
}

static int disposeChild(HANDLE process, HANDLE *thread, HANDLE *processAlias, PROCESS_INFORMATION *original) {
	int result = 1;
	if (process && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
		BOOL terminated = TerminateProcess(process, 97);
		DWORD reaped = WaitForSingleObject(process, 5000);
		fprintf(stderr, "cleanup terminated=%u wait=%lu pid=%lu\n", (unsigned)terminated, (unsigned long)reaped,
				(unsigned long)original->dwProcessId);
		if (!terminated || reaped != WAIT_OBJECT_0)
			result = 0;
	}
	int threadClosed = closeOwned(thread);
	int processClosed = closeOwned(processAlias);
	int originalThreadClosed = closeOwned(&original->hThread);
	int originalProcessClosed = closeOwned(&original->hProcess);
	return result && threadClosed && processClosed && originalThreadClosed && originalProcessClosed;
}

static int testSuspended(BOOL cancel) {
	int result = 1;
	char temporary[MAX_PATH], tlsPath[MAX_PATH] = {0}, entryPath[MAX_PATH] = {0}, releasePath[MAX_PATH] = {0};
	char image[32768], command[32768 + 16];
	PROCESS_INFORMATION original = {0};
	HANDLE thread = NULL, processAlias = NULL, process = NULL;
	STARTUPINFOA startup = {0};
	startup.cb = sizeof(startup);
	DWORD temporaryLength = GetTempPathA(sizeof(temporary), temporary);
	if (!temporaryLength || temporaryLength >= sizeof(temporary) || !GetTempFileNameA(temporary, "stl", 0, tlsPath) ||
		!GetTempFileNameA(temporary, "sen", 0, entryPath) || !GetTempFileNameA(temporary, "sre", 0, releasePath))
		goto cleanup;
	if (!SetEnvironmentVariableA(tlsVariable, tlsPath) || !SetEnvironmentVariableA(entryVariable, entryPath) ||
		!SetEnvironmentVariableA(releaseVariable, releasePath))
		goto cleanup;
	DWORD imageLength = GetModuleFileNameA(NULL, image, sizeof(image));
	if (!imageLength || imageLength >= sizeof(image))
		goto cleanup;
	int formatted = snprintf(command, sizeof(command), "\"%s\" child", image);
	if (formatted <= 0 || (size_t)formatted >= sizeof(command) ||
		!CreateProcessA(image, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &original))
		goto cleanup;
	process = original.hProcess;
	if (!original.hThread || !original.dwProcessId || !original.dwThreadId ||
		GetThreadId(original.hThread) != original.dwThreadId)
		goto cleanup;
	if (!cancel) {
		if (!DuplicateHandle(GetCurrentProcess(), original.hProcess, GetCurrentProcess(), &processAlias, 0, FALSE,
							 DUPLICATE_SAME_ACCESS) ||
			!DuplicateHandle(GetCurrentProcess(), original.hThread, GetCurrentProcess(), &thread, 0, FALSE,
							 DUPLICATE_SAME_ACCESS))
			goto cleanup;
		process = processAlias;
		int originalProcessClosed = closeOwned(&original.hProcess);
		int originalThreadClosed = closeOwned(&original.hThread);
		if (!originalProcessClosed || !originalThreadClosed || GetThreadId(thread) != original.dwThreadId)
			goto cleanup;
	} else {
		thread = original.hThread;
		original.hThread = NULL;
	}
	if (WaitForSingleObject(thread, 150) != WAIT_TIMEOUT || WaitForSingleObject(process, 0) != WAIT_TIMEOUT ||
		!emptyFile(tlsPath) || !emptyFile(entryPath))
		goto cleanup;
	if (cancel) {
		if (!TerminateProcess(process, 77) || WaitForSingleObject(process, 5000) != WAIT_OBJECT_0 ||
			WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !emptyFile(tlsPath) || !emptyFile(entryPath))
			goto cleanup;
		DWORD processCode = 0, threadCode = 0;
		if (!GetExitCodeProcess(process, &processCode) || processCode != 77 ||
			!GetExitCodeThread(thread, &threadCode) || threadCode != 77)
			goto cleanup;
		printf("cancel process=%lu thread=%lu markers=empty\n", (unsigned long)processCode, (unsigned long)threadCode);
	} else {
		SetLastError(0x4321);
		DWORD first = ResumeThread(thread);
		DWORD firstError = GetLastError();
		SetLastError(0x4321);
		DWORD second = ResumeThread(thread);
		DWORD secondError = GetLastError();
		printf("resume first=%lu error=%lu second=%lu error=%lu\n", (unsigned long)first, (unsigned long)firstError,
			   (unsigned long)second, (unsigned long)secondError);
		if (first != 1 || second != 0 || firstError != 0x4321 || secondError != 0x4321)
			goto cleanup;
		struct Marker tls, entry;
		DWORD threadCode = 0;
		if (!waitForMarker(entryPath, &entry) || !readMarker(tlsPath, &tls) || tls.stage != 1 || entry.stage != 2 ||
			tls.threadId != original.dwThreadId || entry.threadId != original.dwThreadId ||
			GetThreadId(thread) != entry.threadId || !GetExitCodeThread(thread, &threadCode) ||
			threadCode != STILL_ACTIVE || WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
			goto cleanup;
		if (!releaseChild(releasePath) || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
			WaitForSingleObject(process, 5000) != WAIT_OBJECT_0)
			goto cleanup;
		DWORD processCode = 0;
		if (!GetExitCodeThread(thread, &threadCode) || threadCode != 42 || !GetExitCodeProcess(process, &processCode) ||
			processCode != 42)
			goto cleanup;
		printf("resumed tls=%lu entry=%lu thread=%lu process=%lu\n", (unsigned long)tls.threadId,
			   (unsigned long)entry.threadId, (unsigned long)threadCode, (unsigned long)processCode);
	}
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "suspended cancel=%u failed error=%lu\n", (unsigned)cancel, (unsigned long)GetLastError());
	if (!disposeChild(process, &thread, &processAlias, &original))
		result = 1;
	int tlsCleared = SetEnvironmentVariableA(tlsVariable, NULL) != FALSE;
	int entryCleared = SetEnvironmentVariableA(entryVariable, NULL) != FALSE;
	int releaseCleared = SetEnvironmentVariableA(releaseVariable, NULL) != FALSE;
	int tlsDeleted = !tlsPath[0] || DeleteFileA(tlsPath);
	int entryDeleted = !entryPath[0] || DeleteFileA(entryPath);
	int releaseDeleted = !releasePath[0] || DeleteFileA(releasePath);
	return result || !tlsCleared || !entryCleared || !releaseCleared || !tlsDeleted || !entryDeleted || !releaseDeleted;
}

int main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "child") == 0)
		return childMain();
	if (argc != 1)
		return 2;
	int resumed = testSuspended(FALSE);
	int canceled = testSuspended(TRUE);
	return resumed || canceled;
}
