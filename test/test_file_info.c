#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include <shellapi.h>

#include "test_assert.h"

static const DWORD seed = 0x13579bdf;
static void die(const char *what) {
	fprintf(stderr, "setup:%s:%lu\n", what, (unsigned long)GetLastError());
	exit(2);
}
static void join(WCHAR *out, const WCHAR *dir, const WCHAR *name) {
	if (wcslen(dir) + wcslen(name) + 2 > MAX_PATH)
		die("length");
	wcscpy(out, dir);
	wcscat(out, L"\\");
	wcscat(out, name);
}
static void save(const WCHAR *path, const void *data, DWORD size) {
	HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		die("create");
	DWORD n = 0;
	if (!WriteFile(f, data, size, &n, NULL) || n != size)
		die("write");
	if (!CloseHandle(f))
		die("close");
}
static void observe(const WCHAR *path, UINT flags, UINT cb, int null_info, DWORD_PTR expected, DWORD expected_error) {
	struct {
		uint64_t before;
		SHFILEINFOW info;
		uint64_t after;
	} out, wanted;
	memset(&out, 0x6b, sizeof(out));
	wanted = out;
	if (path && !null_info) {
		wanted.info.iIcon = 0;
		wanted.info.szDisplayName[0] = 0;
		wanted.info.szTypeName[0] = 0;
	}
	SetLastError(seed);
	DWORD_PTR value = SHGetFileInfoW(path, FILE_ATTRIBUTE_NORMAL, null_info ? NULL : &out.info, cb, flags);
	DWORD error = GetLastError();
	TEST_CHECK_U64_EQ(expected, value);
	TEST_CHECK_EQ(expected_error, error);
	TEST_CHECK(memcmp(&out, &wanted, sizeof(out)) == 0);
}

int main(int argc, char **argv) {
	const int unsupported = argc == 2 && strcmp(argv[1], "unsupported") == 0;
	TEST_CHECK(argc == 1 || unsupported);
	WCHAR self[MAX_PATH], temp[MAX_PATH], root[MAX_PATH], path[MAX_PATH], missing[MAX_PATH], missingdir[MAX_PATH];
	if (!GetModuleFileNameW(NULL, self, MAX_PATH) || !GetTempPathW(MAX_PATH, temp) ||
		!GetTempFileNameW(temp, L"wfi", 0, root) || !DeleteFileW(root) || !CreateDirectoryW(root, NULL))
		die("root");
	HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		die("self");
	DWORD size = GetFileSize(f, NULL), read = 0;
	unsigned char *data = malloc(size), *copy = malloc(size);
	if (!data || !copy || size == INVALID_FILE_SIZE || !ReadFile(f, data, size, &read, NULL) || read != size ||
		!CloseHandle(f))
		die("read");
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)data;
	DWORD pe = (DWORD)dos->e_lfanew;
	if (pe + sizeof(IMAGE_NT_HEADERS) > size)
		die("pe");
	struct mutation {
		const WCHAR *file;
		const char *name;
		int kind;
		DWORD_PTR expected;
	};
	const struct mutation cases[] = {{L"console.exe", "console", 0, 0x4550},
									 {L"gui.exe", "gui", 1, 0x040a4550},
									 {L"gui-wide.exe", "gui-wide", 2, 0x76784550},
									 {L"library.dll", "dll", 3, 0},
									 {L"renamed.data", "renamed", 0, 0x4550},
									 {L"native.exe", "subsystem-native", 4, 0x4550},
									 {L"bad-magic.exe", "bad-magic", 5, 0},
									 {L"bad-machine.exe", "bad-machine", 6, 0},
									 {L"bad-signature.exe", "bad-signature", 7, 0x4d5a},
									 {L"bad-offset.exe", "bad-offset", 8, 0x4d5a},
									 {L"truncated.exe", "truncated", 9, 0},
									 {L"empty.exe", "empty-exe", 10, 0},
									 {L"empty.com", "empty-com", 10, 0},
									 {L"empty.pif", "empty-pif", 10, 0},
									 {L"script.bat", "empty-bat", 10, 0},
									 {L"script.cmd", "empty-cmd", 10, 0},
									 {L"junk.com", "junk-com", 11, 0x4d5a},
									 {L"junk.pif", "junk-pif", 11, 0x4d5a},
									 {L"junk.exe", "junk-exe", 11, 0},
									 {L"dos.exe", "dos", 12, 0x4d5a},
									 {L"win16.exe", "ne-win", 13, 0x040a454e},
									 {L"os16.exe", "ne-os", 14, 0},
									 {L"other16.exe", "ne-other", 15, 0}};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		memcpy(copy, data, size);
		DWORD n = size;
		IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(copy + pe);
		nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
		switch (cases[i].kind) {
		case 1:
			nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
			nt->OptionalHeader.MajorSubsystemVersion = 4;
			nt->OptionalHeader.MinorSubsystemVersion = 10;
			break;
		case 2:
			nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
			nt->OptionalHeader.MajorSubsystemVersion = 0x1234;
			nt->OptionalHeader.MinorSubsystemVersion = 0x5678;
			break;
		case 3:
			nt->FileHeader.Characteristics |= IMAGE_FILE_DLL;
			break;
		case 4:
			nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_NATIVE;
			break;
		case 5:
			nt->OptionalHeader.Magic = 0x999;
			break;
		case 6:
			nt->FileHeader.Machine = 0xffff;
			break;
		case 7:
			nt->Signature = 0x11223344;
			break;
		case 8:
			((IMAGE_DOS_HEADER *)copy)->e_lfanew = -1;
			break;
		case 9:
			n = pe + 40;
			break;
		case 10:
			n = 0;
			break;
		case 11:
			memset(copy, 0x7a, 128);
			n = 128;
			break;
		case 12:
		case 13:
		case 14:
		case 15: {
			memset(copy, 0, 1024);
			n = 1024;
			IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)copy;
			d->e_magic = IMAGE_DOS_SIGNATURE;
			d->e_cblp = 0;
			d->e_cp = 2;
			d->e_cparhdr = 4;
			d->e_lfarlc = 0x40;
			if (cases[i].kind >= 13) {
				d->e_lfanew = 0x80;
				IMAGE_OS2_HEADER *ne = (IMAGE_OS2_HEADER *)(copy + 0x80);
				ne->ne_magic = IMAGE_OS2_SIGNATURE;
				ne->ne_exetyp = cases[i].kind == 13 ? 2 : cases[i].kind == 14 ? 1 : 3;
				ne->ne_expver = 0x040a;
			}
			break;
		}
		}
		join(path, root, cases[i].file);
		save(path, copy, n);
		observe(path, SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, cases[i].expected, 0);
		if (!DeleteFileW(path))
			die("delete");
	}
	observe(NULL, SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, seed);
	observe(self, SHGFI_EXETYPE, 0, 1, 0x4550, 0);
	observe(self, SHGFI_EXETYPE, 0, 0, 0x4550, 0);
	observe(self, SHGFI_EXETYPE, 1, 0, 0x4550, 0);
	observe(self, SHGFI_EXETYPE | SHGFI_DISPLAYNAME, sizeof(SHFILEINFOW), 0, 0, seed);
	observe(L"", SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_ACCESS_DENIED);
	join(missing, root, L"missing.exe");
	observe(missing, SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_FILE_NOT_FOUND);
	join(missingdir, root, L"missing-dir");
	join(missing, missingdir, L"missing.exe");
	observe(missing, SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_PATH_NOT_FOUND);
	observe(root, SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_ACCESS_DENIED);
	f = CreateFileW(self, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		die("lock");
	observe(self, SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_SHARING_VIOLATION);
	CloseHandle(f);
	if (unsupported) {
		observe(self, SHGFI_DISPLAYNAME, sizeof(SHFILEINFOW), 0, 0, ERROR_NOT_SUPPORTED);
		observe(L"C:relative.exe", SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_NOT_SUPPORTED);
		observe(L"Q:\\file.exe", SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_NOT_SUPPORTED);
		observe(L"\\\\server\\share\\file.exe", SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_NOT_SUPPORTED);
		observe(L"\\\\?\\C:\\file.exe", SHGFI_EXETYPE, sizeof(SHFILEINFOW), 0, 0, ERROR_NOT_SUPPORTED);
	}
	if (!RemoveDirectoryW(root))
		die("rmdir");
	free(copy);
	free(data);
	return 0;
}
