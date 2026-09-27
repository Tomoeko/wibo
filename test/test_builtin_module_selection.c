#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "fixture_file_copy.h"

typedef int (*marker_fn)(void);

static int combine(char *output, size_t capacity, const char *directory, const char *name) {
	int length = snprintf(output, capacity, "%s\\%s", directory, name);
	return length > 0 && (size_t)length < capacity;
}

int main(int argc, char **argv) {
	int result = 1, created = 0;
	HMODULE narrow = NULL, wide = NULL;
	char application[MAX_PATH], source[MAX_PATH], destination[MAX_PATH];
	if (argc != 2 || (strcmp(argv[1], "external") && strcmp(argv[1], "builtin")))
		return 2;
	int expectBuiltin = !strcmp(argv[1], "builtin");
	DWORD length = GetModuleFileNameA(NULL, application, sizeof(application));
	if (!length || length >= sizeof(application))
		goto cleanup;
	char *separator = strrchr(application, '\\');
	if (!separator)
		goto cleanup;
	*separator = 0;
	if (!combine(source, sizeof(source), application, "core_runtime_shadow.dll") ||
		!combine(destination, sizeof(destination), application, "version.dll") ||
		!copyOwnedFixtureFile(source, destination, &created))
		goto cleanup;

	narrow = LoadLibraryExA("version.dll", NULL, 0);
	wide = LoadLibraryExW(L"version", NULL, 0);
	if (!narrow || wide != narrow || GetModuleHandleA("version.dll") != narrow || GetModuleHandleA("version") != narrow)
		goto cleanup;
	FARPROC markerAddress = GetProcAddress(narrow, "core_shadow_marker");
	if (expectBuiltin) {
		if (markerAddress || !GetProcAddress(narrow, "GetFileVersionInfoSizeExW"))
			goto cleanup;
	} else {
		marker_fn marker;
		_Static_assert(sizeof(marker) == sizeof(markerAddress), "Function pointer width");
		if (!markerAddress)
			goto cleanup;
		memcpy(&marker, &markerAddress, sizeof(marker));
		if (marker() != 73 || GetProcAddress(narrow, "GetFileVersionInfoSizeExW"))
			goto cleanup;
	}
	printf("builtin module selection: mode=%s passed\n", argv[1]);
	result = 0;
cleanup:
	if (result)
		fprintf(stderr, "builtin module selection failed: error=%lu\n", (unsigned long)GetLastError());
	if (wide && !FreeLibrary(wide))
		result = 1;
	if (narrow && !FreeLibrary(narrow))
		result = 1;
	if (created && !DeleteFileA(destination))
		result = 1;
	return result;
}
