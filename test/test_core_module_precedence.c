#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "fixture_file_copy.h"

static const char *const names[] = {"kernel32.dll", "kernelbase.dll", "ntdll.dll"};
static const WCHAR *const wideNames[] = {L"kernel32.dll", L"kernelbase.dll", L"ntdll.dll"};
static const char *const exports[] = {"GetCurrentProcessId", "GetCurrentProcessId", "RtlNtStatusToDosError"};

static int appendPath(char *output, size_t capacity, const char *directory, const char *name) {
	int length = snprintf(output, capacity, "%s\\%s", directory, name);
	return length > 0 && (size_t)length < capacity;
}

static int checkCore(HMODULE original[3], FARPROC functions[3], const char *phase) {
	for (unsigned index = 0; index < 3; ++index) {
		HMODULE narrow = LoadLibraryExA(names[index], NULL, 0);
		HMODULE wide = LoadLibraryExW(wideNames[index], NULL, 0);
		int correct = narrow == original[index] && wide == original[index] &&
					  GetProcAddress(narrow, exports[index]) == functions[index] &&
					  GetProcAddress(narrow, "core_shadow_marker") == NULL &&
					  GetModuleHandleA(names[index]) == original[index];
		printf("phase=%s module=%s same=%d\n", phase, names[index], correct);
		if (wide && !FreeLibrary(wide))
			correct = 0;
		if (narrow && !FreeLibrary(narrow))
			correct = 0;
		if (!correct)
			return 0;
	}
	return 1;
}

#define CHECK(condition)                                                                                               \
	do {                                                                                                               \
		if (!(condition)) {                                                                                            \
			fprintf(stderr, "core module fixture failed at line %d: error=%lu\n", __LINE__,                            \
					(unsigned long)GetLastError());                                                                    \
			goto cleanup;                                                                                              \
		}                                                                                                              \
	} while (0)

int main(void) {
	int result = 1, temporaryCreated = 0, directoryCreated = 0, directoryChanged = 0;
	int applicationCreated[3] = {0}, userCreated[3] = {0};
	HMODULE original[3];
	FARPROC functions[3];
	char application[MAX_PATH], source[MAX_PATH], user[MAX_PATH] = {0}, temporary[MAX_PATH], oldDirectory[MAX_PATH];
	char applicationFiles[3][MAX_PATH], userFiles[3][MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, application, sizeof(application));
	CHECK(length && length < sizeof(application));
	char *separator = strrchr(application, '\\');
	CHECK(separator);
	*separator = 0;
	CHECK(appendPath(source, sizeof(source), application, "core_runtime_shadow.dll"));
	length = GetDllDirectoryA(sizeof(oldDirectory), oldDirectory);
	CHECK(length < sizeof(oldDirectory));
	oldDirectory[length] = 0;
	for (unsigned index = 0; index < 3; ++index) {
		original[index] = GetModuleHandleA(names[index]);
		CHECK(original[index]);
		functions[index] = GetProcAddress(original[index], exports[index]);
		CHECK(functions[index]);
		CHECK(GetProcAddress(original[index], "core_shadow_marker") == NULL);
	}
	length = GetTempPathA(sizeof(temporary), temporary);
	CHECK(length && length < sizeof(temporary));
	CHECK(GetTempFileNameA(temporary, "cmo", 0, user));
	temporaryCreated = 1;
	CHECK(DeleteFileA(user));
	temporaryCreated = 0;
	CHECK(CreateDirectoryA(user, NULL));
	directoryCreated = 1;
	for (unsigned index = 0; index < 3; ++index) {
		CHECK(appendPath(applicationFiles[index], MAX_PATH, application, names[index]));
		CHECK(copyOwnedFixtureFile(source, applicationFiles[index], &applicationCreated[index]));
	}
	CHECK(checkCore(original, functions, "application-shadow"));
	for (unsigned index = 0; index < 3; ++index) {
		CHECK(DeleteFileA(applicationFiles[index]));
		applicationCreated[index] = 0;
		CHECK(appendPath(userFiles[index], MAX_PATH, user, names[index]));
		CHECK(copyOwnedFixtureFile(source, userFiles[index], &userCreated[index]));
	}
	CHECK(SetDllDirectoryA(user));
	directoryChanged = 1;
	CHECK(checkCore(original, functions, "user-shadow"));
	result = 0;
cleanup:
	if (directoryChanged && !SetDllDirectoryA(oldDirectory[0] ? oldDirectory : NULL))
		result = 1;
	for (unsigned index = 0; index < 3; ++index) {
		if (applicationCreated[index] && !DeleteFileA(applicationFiles[index]))
			result = 1;
		if (userCreated[index] && !DeleteFileA(userFiles[index]))
			result = 1;
	}
	if (directoryCreated && !RemoveDirectoryA(user))
		result = 1;
	if (temporaryCreated && !DeleteFileA(user))
		result = 1;
	return result;
}
