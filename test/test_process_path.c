#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define IMAGE_CAPACITY 32768
static const WCHAR expectedVariable[] = L"PATH_PROBE_EXPECTED_IMAGE";
static const WCHAR tokenVariable[] = L"PATH_PROBE_TOKEN";

static int closeOwned(HANDLE *handle) {
	if (!*handle || *handle == INVALID_HANDLE_VALUE)
		return 1;
	HANDLE owned = *handle;
	*handle = NULL;
	return CloseHandle(owned) != FALSE;
}

static int finishChild(PROCESS_INFORMATION *process, DWORD expected, DWORD timeout) {
	int result = 1;
	DWORD code = 0;
	DWORD waited = WaitForSingleObject(process->hProcess, timeout);
	if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(process->hProcess, &code) && code == expected)
		result = 0;
	if (result) {
		fprintf(stderr, "child pid=%lu wait=%lu code=%lu error=%lu\n", (unsigned long)process->dwProcessId,
				(unsigned long)waited, (unsigned long)code, (unsigned long)GetLastError());
		if (waited != WAIT_OBJECT_0) {
			BOOL terminated = TerminateProcess(process->hProcess, 101);
			DWORD reaped = WaitForSingleObject(process->hProcess, 5000);
			fprintf(stderr, "cleanup terminated=%u wait=%lu\n", (unsigned)terminated, (unsigned long)reaped);
			if (!terminated || reaped != WAIT_OBJECT_0)
				result = 1;
		}
	}
	int threadClosed = closeOwned(&process->hThread);
	int processClosed = closeOwned(&process->hProcess);
	return result || !threadClosed || !processClosed;
}

static int leafMain(void) {
	WCHAR actual[IMAGE_CAPACITY], expected[IMAGE_CAPACITY], token[16];
	DWORD length = GetModuleFileNameW(NULL, actual, IMAGE_CAPACITY);
	DWORD expectedLength = GetEnvironmentVariableW(expectedVariable, expected, IMAGE_CAPACITY);
	DWORD tokenLength = GetEnvironmentVariableW(tokenVariable, token, 16);
	if (!length || length >= IMAGE_CAPACITY || expectedLength != length || expectedLength >= IMAGE_CAPACITY ||
		tokenLength != 5 || memcmp(token, L"owned", 6 * sizeof(WCHAR)) || _wcsicmp(actual, expected))
		return 31;
	return 42;
}

static BOOL createBare(BOOL wide, const char *name, PROCESS_INFORMATION *process) {
	char commandA[128];
	WCHAR commandW[128];
	STARTUPINFOA startupA = {0};
	STARTUPINFOW startupW = {0};
	startupA.cb = sizeof(startupA);
	startupW.cb = sizeof(startupW);
	int count = snprintf(commandA, sizeof(commandA), "%s --leaf", name);
	if (count <= 0 || (size_t)count >= sizeof(commandA))
		return FALSE;
	for (int index = 0; index <= count; ++index)
		commandW[index] = (WCHAR)(unsigned char)commandA[index];
	return wide ? CreateProcessW(NULL, commandW, NULL, NULL, FALSE, 0, NULL, NULL, &startupW, process)
				: CreateProcessA(NULL, commandA, NULL, NULL, FALSE, 0, NULL, NULL, &startupA, process);
}

static int resolverMain(BOOL wide, const char *name) {
	WCHAR path[IMAGE_CAPACITY];
	DWORD length = GetEnvironmentVariableW(L"PATH", path, IMAGE_CAPACITY);
	if (length < 3 || length >= IMAGE_CAPACITY || path[1] != ':' || wcschr(path, L';'))
		return 21;
	// The copied image is absent from the application/current directories and system search path.
	if (!SetEnvironmentVariableW(L"PATH", NULL))
		return 22;
	PROCESS_INFORMATION negative = {0};
	SetLastError(0);
	if (createBare(wide, name, &negative)) {
		(void)finishChild(&negative, 42, 5000);
		return 23;
	}
	if (GetLastError() != ERROR_FILE_NOT_FOUND)
		return 24;
	if (!SetEnvironmentVariableW(L"PATH", path))
		return 25;
	PROCESS_INFORMATION process = {0};
	if (!createBare(wide, name, &process)) {
		fprintf(stderr, "bare CreateProcess%c failed error=%lu\n", wide ? 'W' : 'A', (unsigned long)GetLastError());
		return 26;
	}
	return finishChild(&process, 42, 5000) ? 27 : 0;
}

static int appendEntry(WCHAR *block, size_t capacity, size_t *used, const WCHAR *name, const WCHAR *value) {
	size_t nameLength = wcslen(name), valueLength = wcslen(value);
	if (*used >= capacity || nameLength + valueLength + 2 > capacity - *used)
		return 0;
	memcpy(block + *used, name, nameLength * sizeof(WCHAR));
	*used += nameLength;
	block[(*used)++] = '=';
	memcpy(block + *used, value, (valueLength + 1) * sizeof(WCHAR));
	*used += valueLength + 1;
	return 1;
}

static int copyOwnedImage(const WCHAR *source, const WCHAR *destination) {
	HANDLE input = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (input == INVALID_HANDLE_VALUE)
		return 0;
	HANDLE output = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	int copied = output != INVALID_HANDLE_VALUE;
	unsigned char bytes[65536];
	size_t total = 0;
	while (copied) {
		DWORD count = 0, written = 0;
		if (!ReadFile(input, bytes, sizeof(bytes), &count, NULL)) {
			copied = 0;
			break;
		}
		if (!count)
			break;
		if (total > 16 * 1024 * 1024 - count || !WriteFile(output, bytes, count, &written, NULL) || written != count) {
			copied = 0;
			break;
		}
		total += count;
	}
	int existed = output != INVALID_HANDLE_VALUE;
	int inputClosed = closeOwned(&input), outputClosed = closeOwned(&output);
	copied = copied && total && inputClosed && outputClosed;
	if (!copied && existed)
		(void)DeleteFileW(destination);
	return copied;
}

static int runResolver(const WCHAR *image, const WCHAR *directory, const WCHAR *copiedImage, const char *name,
					   BOOL wide) {
	WCHAR environment[3 * IMAGE_CAPACITY], command[IMAGE_CAPACITY + 160];
	WCHAR wideName[128];
	size_t nameLength = strlen(name), used = 0;
	if (nameLength >= 128)
		return 1;
	for (size_t index = 0; index <= nameLength; ++index)
		wideName[index] = (WCHAR)(unsigned char)name[index];
	if (!appendEntry(environment, 3 * IMAGE_CAPACITY, &used, L"PATH", directory) ||
		!appendEntry(environment, 3 * IMAGE_CAPACITY, &used, expectedVariable, copiedImage) ||
		!appendEntry(environment, 3 * IMAGE_CAPACITY, &used, tokenVariable, L"owned") || used >= 3 * IMAGE_CAPACITY)
		return 1;
	environment[used] = 0;
	int length =
		swprintf(command, IMAGE_CAPACITY + 160, L"\"%ls\" --resolve-%lc %ls", image, wide ? L'w' : L'a', wideName);
	if (length <= 0 || length >= IMAGE_CAPACITY + 160)
		return 1;
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof(startup);
	if (!CreateProcessW(image, command, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, environment, NULL, &startup,
						&process)) {
		fprintf(stderr, "resolver spawn failed error=%lu\n", (unsigned long)GetLastError());
		return 1;
	}
	return finishChild(&process, 0, 25000);
}

static int parentMain(void) {
	int result = 1;
	WCHAR image[IMAGE_CAPACITY], temporary[MAX_PATH], directory[MAX_PATH] = {0}, copiedImage[MAX_PATH] = {0};
	WCHAR oldPath[IMAGE_CAPACITY], wideName[128];
	char name[128];
	int pathCaptured = 0, hadPath = 0, directoryCreated = 0, imageCopied = 0, temporaryCreated = 0;
	oldPath[0] = 0;
	SetLastError(0);
	DWORD oldLength = GetEnvironmentVariableW(L"PATH", oldPath, IMAGE_CAPACITY);
	if (oldLength >= IMAGE_CAPACITY)
		goto cleanup;
	hadPath = GetLastError() != ERROR_ENVVAR_NOT_FOUND;
	pathCaptured = 1;
	DWORD imageLength = GetModuleFileNameW(NULL, image, IMAGE_CAPACITY);
	DWORD temporaryLength = GetTempPathW(MAX_PATH, temporary);
	if (!imageLength || imageLength >= IMAGE_CAPACITY || !temporaryLength || temporaryLength >= MAX_PATH ||
		!GetTempFileNameW(temporary, L"pth", 0, directory))
		goto cleanup;
	temporaryCreated = 1;
	if (!DeleteFileW(directory))
		goto cleanup;
	temporaryCreated = 0;
	if (!CreateDirectoryW(directory, NULL))
		goto cleanup;
	directoryCreated = 1;
	int nameLength = snprintf(name, sizeof(name), "path-probe-%lu-child.exe", (unsigned long)GetCurrentProcessId());
	if (nameLength <= 0 || (size_t)nameLength >= sizeof(name))
		goto cleanup;
	for (int index = 0; index <= nameLength; ++index)
		wideName[index] = (WCHAR)(unsigned char)name[index];
	int copiedLength = swprintf(copiedImage, MAX_PATH, L"%ls\\%ls", directory, wideName);
	if (copiedLength <= 0 || copiedLength >= MAX_PATH || !copyOwnedImage(image, copiedImage))
		goto cleanup;
	imageCopied = 1;
	if (!SetEnvironmentVariableW(L"PATH", NULL))
		goto cleanup;
	SetLastError(0);
	if (GetEnvironmentVariableW(L"PATH", NULL, 0) || GetLastError() != ERROR_ENVVAR_NOT_FOUND)
		goto cleanup;
	if (runResolver(image, directory, copiedImage, name, FALSE) ||
		runResolver(image, directory, copiedImage, name, TRUE))
		goto cleanup;
	SetLastError(0);
	if (GetEnvironmentVariableW(L"PATH", NULL, 0) || GetLastError() != ERROR_ENVVAR_NOT_FOUND)
		goto cleanup;
	puts("single-directory guest PATH: CreateProcessA/W resolved owned image");
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "PATH fixture failed error=%lu\n", (unsigned long)GetLastError());
	int pathRestored = !pathCaptured || SetEnvironmentVariableW(L"PATH", hadPath ? oldPath : NULL);
	int imageDeleted = !imageCopied || DeleteFileW(copiedImage);
	int directoryDeleted = !directoryCreated || RemoveDirectoryW(directory);
	int temporaryDeleted = !temporaryCreated || DeleteFileW(directory);
	return result || !pathRestored || !imageDeleted || !directoryDeleted || !temporaryDeleted;
}

int main(int argc, char **argv) {
	if (argc == 2 && !strcmp(argv[1], "--leaf"))
		return leafMain();
	if (argc == 3 && !strcmp(argv[1], "--resolve-a"))
		return resolverMain(FALSE, argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--resolve-w"))
		return resolverMain(TRUE, argv[2]);
	return argc == 1 ? parentMain() : 2;
}
