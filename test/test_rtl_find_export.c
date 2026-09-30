#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef void *(WINAPI *find_export_fn)(HMODULE, const char *);

static void check_mapped_image(find_export_fn findExport, const char *fileName, const char *exportName,
							   BOOL forwarded) {
	char path[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
	TEST_CHECK(length != 0 && length < sizeof(path));
	char *baseName = path;
	for (char *cursor = path; *cursor; ++cursor) {
		if (*cursor == '\\' || *cursor == '/')
			baseName = cursor + 1;
	}
	TEST_CHECK(strlen(fileName) < sizeof(path) - (size_t)(baseName - path));
	strcpy(baseName, fileName);
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
							  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	HANDLE mapping = CreateFileMappingA(file, NULL, SEC_IMAGE | PAGE_READONLY, 0, 0, NULL);
	TEST_CHECK(mapping != NULL);
	BYTE *image = (BYTE *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	TEST_CHECK(image != NULL);
	void *result = findExport((HMODULE)image, exportName);
	if (forwarded) {
		TEST_CHECK(result == NULL);
	} else {
		const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
		TEST_CHECK(dos->e_magic == IMAGE_DOS_SIGNATURE);
		const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(image + dos->e_lfanew);
		TEST_CHECK(nt->Signature == IMAGE_NT_SIGNATURE);
		TEST_CHECK(result != NULL);
		TEST_CHECK((BYTE *)result >= image && (BYTE *)result < image + nt->OptionalHeader.SizeOfImage);
		TEST_CHECK(findExport((HMODULE)image, "missing_export") == NULL);
	}
	TEST_CHECK(UnmapViewOfFile(image));
	TEST_CHECK(CloseHandle(mapping));
	TEST_CHECK(CloseHandle(file));
}

static void check_truncated_image(find_export_fn findExport) {
	BYTE *image = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	TEST_CHECK(image != NULL);
	*(WORD *)image = IMAGE_DOS_SIGNATURE;
	*(LONG *)(image + 0x3c) = 0xff8;
	*(DWORD *)(image + 0xff8) = IMAGE_NT_SIGNATURE;
	TEST_CHECK(findExport((HMODULE)image, "fixture_marker") == NULL);
	TEST_CHECK(VirtualFree(image, 0, MEM_RELEASE));
}

int main(int argc, char **argv) {
	HMODULE native = GetModuleHandleA("ntdll.dll");
	TEST_CHECK(native != NULL);
	find_export_fn findExport = (find_export_fn)(void *)GetProcAddress(native, "RtlFindExportedRoutineByName");
	TEST_CHECK(findExport != NULL);
	if (argc > 1 && strcmp(argv[1], "bounded") == 0) {
		check_truncated_image(findExport);
		return 0;
	}

	HMODULE direct = LoadLibraryA("external_exports.dll");
	TEST_CHECK(direct != NULL);
#ifdef _WIN64
	const char *directName = "add_numbers";
#else
	const char *directName = "add_numbers@8";
#endif
	void *expected = (void *)GetProcAddress(direct, directName);
	TEST_CHECK(expected != NULL);
	TEST_CHECK(findExport(direct, directName) == expected);
	TEST_CHECK(findExport(direct, "missing_export") == NULL);
	TEST_CHECK(findExport(direct, "") == NULL);
	TEST_CHECK(FreeLibrary(direct));

	HMODULE forwarder = LoadLibraryA("system_loader_forwarder.dll");
	TEST_CHECK(forwarder != NULL);
	TEST_CHECK(GetProcAddress(forwarder, "forward_value") != NULL);
	TEST_CHECK(findExport(forwarder, "forward_value") == NULL);
	TEST_CHECK(FreeLibrary(forwarder));

	check_mapped_image(findExport, "resource_only_fixture.dll", "fixture_marker", FALSE);
	check_mapped_image(findExport, "system_loader_forwarder.dll", "forward_value", TRUE);

	HMODULE imageResource = LoadLibraryExA("resource_only_fixture.dll", NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	TEST_CHECK(imageResource != NULL);
	HMODULE imageBase = (HMODULE)((uintptr_t)imageResource & ~(uintptr_t)3);
	TEST_CHECK(findExport(imageBase, "fixture_marker") != NULL);
	TEST_CHECK(findExport(imageBase, "missing_export") == NULL);
	TEST_CHECK(FreeLibrary(imageResource));
	return 0;
}
