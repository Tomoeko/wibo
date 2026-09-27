#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_file_copy.h"
#include "test_assert.h"

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE value = *handle;
	*handle = INVALID_HANDLE_VALUE;
	return CloseHandle(value) != FALSE;
}

static int combine(char *output, size_t capacity, const char *directory, const char *name) {
	int length = snprintf(output, capacity, "%s\\%s", directory, name);
	return length > 0 && (size_t)length < capacity;
}

static int childMain(const char *expected) {
	char image[32768];
	DWORD length = GetModuleFileNameA(NULL, image, sizeof(image));
	if (!length || length >= sizeof(image))
		return 21;
	HANDLE actualFile = CreateFileA(image, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
									OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	HANDLE expectedFile = CreateFileA(expected, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
									  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	BY_HANDLE_FILE_INFORMATION actual, wanted;
	int match = actualFile != INVALID_HANDLE_VALUE && expectedFile != INVALID_HANDLE_VALUE &&
				GetFileInformationByHandle(actualFile, &actual) && GetFileInformationByHandle(expectedFile, &wanted) &&
				actual.dwVolumeSerialNumber == wanted.dwVolumeSerialNumber &&
				actual.nFileIndexHigh == wanted.nFileIndexHigh && actual.nFileIndexLow == wanted.nFileIndexLow;
	int actualClosed = closeOwned(&actualFile), expectedClosed = closeOwned(&expectedFile);
	return match && actualClosed && expectedClosed ? 0 : 22;
}

static int runChild(const char *name, const char *expected) {
	char command[32768];
	int length = snprintf(command, sizeof(command), "%s --child \"%s\"", name, expected);
	STARTUPINFOA startup = {0};
	PROCESS_INFORMATION child = {0};
	startup.cb = sizeof(startup);
	if (length <= 0 || (size_t)length >= sizeof(command) ||
		!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child)) {
		fprintf(stderr, "executable search failed: error=%lu\n", (unsigned long)GetLastError());
		return 0;
	}
	DWORD waited = WaitForSingleObject(child.hProcess, 5000), exitCode = 0;
	int result = waited == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &exitCode) && exitCode == 0;
	if (!result) {
		fprintf(stderr, "search child: wait=%lu code=%lu pid=%lu\n", (unsigned long)waited, (unsigned long)exitCode,
				(unsigned long)child.dwProcessId);
		if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0) {
			BOOL terminated = TerminateProcess(child.hProcess, 32);
			DWORD reaped = WaitForSingleObject(child.hProcess, 5000);
			fprintf(stderr, "search cleanup: terminate=%ld wait=%lu pid=%lu\n", (long)terminated, (unsigned long)reaped,
					(unsigned long)child.dwProcessId);
		}
	}
	int threadClosed = closeOwned(&child.hThread), processClosed = closeOwned(&child.hProcess);
	return result && threadClosed && processClosed;
}

static int testSearchOrder(void) {
	int result = 0, temporaryFileCreated = 0, rootCreated = 0, pathCreated = 0, legacyCreated = 0;
	int currentChanged = 0, pathChanged = 0;
	char image[32768], application[32768], originalCurrent[32768], temporary[MAX_PATH], root[MAX_PATH] = {0};
	char system[MAX_PATH], windows[MAX_PATH], legacy[MAX_PATH], pathDirectory[MAX_PATH], name[128];
	char destinations[6][32768] = {{0}};
	int created[6] = {0};
	char *originalPath = NULL;
	DWORD imageLength = GetModuleFileNameA(NULL, image, sizeof(image));
	DWORD currentLength = GetCurrentDirectoryA(sizeof(originalCurrent), originalCurrent);
	UINT systemLength = GetSystemDirectoryA(system, sizeof(system)),
		 windowsLength = GetWindowsDirectoryA(windows, sizeof(windows));
	DWORD temporaryLength = GetTempPathA(sizeof(temporary), temporary);
	if (!imageLength || imageLength >= sizeof(image) || !currentLength || currentLength >= sizeof(originalCurrent) ||
		!systemLength || systemLength >= sizeof(system) || !windowsLength || windowsLength >= sizeof(windows) ||
		!temporaryLength || temporaryLength >= sizeof(temporary))
		goto cleanup;
	memcpy(application, image, imageLength + 1);
	char *slash = strrchr(application, '\\'), *forwardSlash = strrchr(application, '/');
	if (!slash || (forwardSlash && forwardSlash > slash))
		slash = forwardSlash;
	if (!slash)
		goto cleanup;
	*slash = 0;
	SetLastError(ERROR_SUCCESS);
	DWORD pathLength = GetEnvironmentVariableA("PATH", NULL, 0);
	if (pathLength) {
		originalPath = malloc(pathLength);
		if (!originalPath || GetEnvironmentVariableA("PATH", originalPath, pathLength) >= pathLength)
			goto cleanup;
	} else if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
		originalPath = malloc(1);
		if (!originalPath)
			goto cleanup;
		originalPath[0] = 0;
	}
	if (!GetTempFileNameA(temporary, "pse", 0, root))
		goto cleanup;
	temporaryFileCreated = 1;
	if (!DeleteFileA(root))
		goto cleanup;
	temporaryFileCreated = 0;
	if (!CreateDirectoryA(root, NULL))
		goto cleanup;
	rootCreated = 1;
	if (!combine(pathDirectory, sizeof(pathDirectory), root, "path") || !CreateDirectoryA(pathDirectory, NULL))
		goto cleanup;
	pathCreated = 1;
	if (!combine(legacy, sizeof(legacy), windows, "System"))
		goto cleanup;
	if (GetFileAttributesA(legacy) == INVALID_FILE_ATTRIBUTES) {
		if (!CreateDirectoryA(legacy, NULL))
			goto cleanup;
		legacyCreated = 1;
	}
	int nameLength = snprintf(name, sizeof(name), "process-search-%lu.exe", (unsigned long)GetCurrentProcessId());
	if (nameLength <= 0 || (size_t)nameLength >= sizeof(name))
		goto cleanup;
	const char *directories[] = {application, root, system, legacy, windows, pathDirectory};
	for (unsigned index = 0; index < 6; ++index)
		if (!combine(destinations[index], sizeof(destinations[index]), directories[index], name) ||
			!copyOwnedFixtureFile(image, destinations[index], &created[index]))
			goto cleanup;
	if (!SetCurrentDirectoryA(root))
		goto cleanup;
	currentChanged = 1;
	if (!SetEnvironmentVariableA("PATH", pathDirectory))
		goto cleanup;
	pathChanged = 1;
	for (unsigned index = 0; index < 6; ++index) {
		if (!runChild(name, destinations[index]) || !DeleteFileA(destinations[index]))
			goto cleanup;
		created[index] = 0;
	}
	result = 1;
cleanup:
	if (!result)
		fprintf(stderr, "search-order fixture failed: error=%lu\n", (unsigned long)GetLastError());
	if (pathChanged && !SetEnvironmentVariableA("PATH", originalPath))
		result = 0;
	if (currentChanged && !SetCurrentDirectoryA(originalCurrent))
		result = 0;
	for (unsigned index = 0; index < 6; ++index)
		if (created[index] && !DeleteFileA(destinations[index]))
			result = 0;
	if (pathCreated && !RemoveDirectoryA(pathDirectory))
		result = 0;
	if (rootCreated && !RemoveDirectoryA(root))
		result = 0;
	if (temporaryFileCreated && !DeleteFileA(root))
		result = 0;
	if (legacyCreated && !RemoveDirectoryA(legacy))
		result = 0;
	free(originalPath);
	return result;
}

int main(int argc, char **argv) {
	if (argc == 3 && !strcmp(argv[1], "--child"))
		return childMain(argv[2]);
	TEST_CHECK(testSearchOrder());
	return 0;
}
