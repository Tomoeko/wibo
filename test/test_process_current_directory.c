#include "test_assert.h"
#include <stdio.h>
#include <windows.h>

static char modulePath[MAX_PATH];
static char parentDirectory[MAX_PATH];
static BOOL observe;

static void appendPath(char *result, size_t capacity, const char *base, const char *suffix) {
	const size_t baseLength = strlen(base), suffixLength = strlen(suffix);
	TEST_CHECK(baseLength + suffixLength < capacity);
	memcpy(result, base, baseLength);
	memcpy(result + baseLength, suffix, suffixLength + 1);
}

static BOOL launch(const char *application, const char *directory, const char *expected, BOOL wide, BOOL writeMarker,
				   DWORD *error) {
	char command[3 * MAX_PATH + 40];
	snprintf(command, sizeof(command), "\"%s\" %s \"%s\\.\"", modulePath, writeMarker ? "child-write" : "child",
			 expected);
	PROCESS_INFORMATION process = {0};
	BOOL result;
	SetLastError(0x20001234);
	if (wide) {
		WCHAR applicationW[MAX_PATH], commandW[3 * MAX_PATH + 40], directoryW[MAX_PATH];
		TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, application, -1, applicationW, MAX_PATH));
		TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, command, -1, commandW, sizeof(commandW) / sizeof(*commandW)));
		if (directory)
			TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, directory, -1, directoryW, MAX_PATH));
		STARTUPINFOW startup = {0};
		startup.cb = sizeof(startup);
		result = CreateProcessW(applicationW, commandW, NULL, NULL, FALSE, 0, NULL, directory ? directoryW : NULL,
								&startup, &process);
	} else {
		STARTUPINFOA startup = {0};
		startup.cb = sizeof(startup);
		result = CreateProcessA(application, command, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process);
	}
	*error = GetLastError();
	if (result) {
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 5000));
		DWORD exitCode = 0;
		TEST_CHECK(GetExitCodeProcess(process.hProcess, &exitCode));
		TEST_CHECK_EQ(0, exitCode);
		TEST_CHECK(CloseHandle(process.hThread));
		TEST_CHECK(CloseHandle(process.hProcess));
	}
	char current[MAX_PATH];
	TEST_CHECK(GetCurrentDirectoryA(sizeof(current), current));
	TEST_CHECK(strcmp(current, parentDirectory) == 0);
	return result;
}

static void invalidDirectory(const char *name, const char *application, const char *directory, DWORD expectedError) {
	for (BOOL wide = FALSE; wide <= TRUE; ++wide) {
		DWORD error = 0;
		BOOL result = launch(application, directory, parentDirectory, wide, FALSE, &error);
		if (observe)
			printf("%s %c result=%u error=%lu\n", name, wide ? 'W' : 'A', result, error);
		else {
			TEST_CHECK(!result);
			TEST_CHECK_EQ(expectedError, error);
		}
	}
}

int main(int argc, char **argv) {
	if (argc >= 3 && (strcmp(argv[1], "child") == 0 || strcmp(argv[1], "child-write") == 0)) {
		char current[MAX_PATH], expected[MAX_PATH];
		TEST_CHECK(GetCurrentDirectoryA(sizeof(current), current));
		TEST_CHECK(GetFullPathNameA(argv[2], sizeof(expected), expected, NULL));
		TEST_CHECK_MSG(_stricmp(current, expected) == 0, "Current directory '%s' differs from expected '%s'", current,
					   expected);
		if (strcmp(argv[1], "child-write") == 0) {
			HANDLE marker =
				CreateFileA("cwd-relative-marker.tmp", GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
			TEST_CHECK(marker != INVALID_HANDLE_VALUE);
			TEST_CHECK(CloseHandle(marker));
		}
		return 0;
	}
	observe = argc > 1 && strcmp(argv[1], "observe") == 0;
	TEST_CHECK(GetModuleFileNameA(NULL, modulePath, sizeof(modulePath)));
	TEST_CHECK(GetCurrentDirectoryA(sizeof(parentDirectory), parentDirectory));
	char temporary[MAX_PATH], seed[MAX_PATH], childDirectory[MAX_PATH], missingDirectory[MAX_PATH],
		missingImage[MAX_PATH];
	TEST_CHECK(GetTempPathA(sizeof(temporary), temporary));
	TEST_CHECK(GetTempFileNameA(temporary, "cwd", 0, seed));
	appendPath(childDirectory, sizeof(childDirectory), seed, "-directory with space");
	TEST_CHECK(CreateDirectoryA(childDirectory, NULL));
	appendPath(missingDirectory, sizeof(missingDirectory), childDirectory, "\\missing");
	appendPath(missingImage, sizeof(missingImage), childDirectory, "\\missing.exe");
	for (BOOL wide = FALSE; wide <= TRUE; ++wide) {
		DWORD error = 0;
		TEST_CHECK(launch(modulePath, NULL, parentDirectory, wide, FALSE, &error));
		TEST_CHECK(launch(modulePath, childDirectory, childDirectory, wide, TRUE, &error));
		char marker[MAX_PATH];
		appendPath(marker, sizeof(marker), childDirectory, "\\cwd-relative-marker.tmp");
		TEST_CHECK(GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES);
		TEST_CHECK(DeleteFileA(marker));
		BOOL empty = launch(modulePath, "", parentDirectory, wide, FALSE, &error);
		if (observe)
			printf("empty %c result=%u error=%lu\n", wide ? 'W' : 'A', empty, error);
		else {
			TEST_CHECK(!empty);
			TEST_CHECK_EQ(ERROR_DIRECTORY, error);
		}
	}
	invalidDirectory("missing-directory", modulePath, missingDirectory, ERROR_DIRECTORY);
	invalidDirectory("file-directory", modulePath, seed, ERROR_DIRECTORY);
	invalidDirectory("missing-image", missingImage, childDirectory, ERROR_FILE_NOT_FOUND);
	invalidDirectory("missing-image-and-directory", missingImage, missingDirectory, ERROR_DIRECTORY);
	invalidDirectory("missing-image-and-file-directory", missingImage, seed, ERROR_DIRECTORY);
	TEST_CHECK(DeleteFileA(seed));
	TEST_CHECK(RemoveDirectoryA(childDirectory));
	return 0;
}
