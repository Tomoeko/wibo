#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

typedef BOOL(WINAPI *SetInformationFn)(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, void *data, DWORD length);

static void make_name(char *name) {
	char directory[MAX_PATH];
	DWORD count = GetTempPathA(MAX_PATH, directory);
	TEST_CHECK(count > 0 && count < MAX_PATH);
	TEST_CHECK(GetTempFileNameA(directory, "wbi", 0, name) != 0);
}

static HANDLE open_file(const char *name, DWORD access) {
	HANDLE file = CreateFileA(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	return file;
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC procedure = GetProcAddress(kernel, "SetFileInformationByHandle");
	TEST_CHECK(procedure != NULL);
	SetInformationFn update;
	TEST_CHECK(sizeof(update) == sizeof(procedure));
	memcpy(&update, &procedure, sizeof(update));
	char name[MAX_PATH];
	make_name(name);
	HANDLE file = open_file(name, GENERIC_READ | GENERIC_WRITE | DELETE);
	FILE_DISPOSITION_INFO disposition = {FALSE};
	SetLastError(0x7315);
	TEST_CHECK(update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
	TEST_CHECK_EQ(0x7315, GetLastError());
	disposition.DeleteFile = TRUE;
	TEST_CHECK(update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
	for (unsigned attempt = 0; attempt < 2; ++attempt) {
		DWORD access = attempt ? GENERIC_READ : 0;
		HANDLE blocked = CreateFileA(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
									 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (getenv("WIBO_FIXTURE_RUNTIME")) {
			TEST_CHECK(blocked == INVALID_HANDLE_VALUE);
			TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
		} else if (blocked == INVALID_HANDLE_VALUE) {
			TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
		} else {
			fprintf(stderr, "Baseline allows a new open while delete is pending (access=%lu)\n", (unsigned long)access);
			TEST_CHECK(CloseHandle(blocked));
		}
	}
	disposition.DeleteFile = FALSE;
	TEST_CHECK(update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
	HANDLE reopened = open_file(name, GENERIC_READ);
	TEST_CHECK(CloseHandle(reopened));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES);
	TEST_CHECK(DeleteFileA(name));

	make_name(name);
	file = open_file(name, GENERIC_READ | GENERIC_WRITE | DELETE);
	HANDLE duplicate = NULL;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	disposition.DeleteFile = TRUE;
	TEST_CHECK(update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
	TEST_CHECK(CloseHandle(file));
	const char payload[] = "retained file";
	DWORD written = 0;
	TEST_CHECK(WriteFile(duplicate, payload, sizeof(payload), &written, NULL));
	TEST_CHECK_EQ(sizeof(payload), written);
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES);
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());

	make_name(name);
	file = open_file(name, GENERIC_READ | GENERIC_WRITE);
	for (unsigned value = 0; value < 2; ++value) {
		disposition.DeleteFile = (BOOLEAN)value;
		SetLastError(0x7315);
		TEST_CHECK(!update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
		TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	}
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(DeleteFileA(name));

	make_name(name);
	file = open_file(name, GENERIC_READ | GENERIC_WRITE | DELETE);
	disposition.DeleteFile = TRUE;
	TEST_CHECK(!update(file, FileDispositionInfo, &disposition, 0));
	TEST_CHECK(!update(file, (FILE_INFO_BY_HANDLE_CLASS)0x7fffffff, &disposition, sizeof(disposition)));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(!update((HANDLE)(ULONG_PTR)0x12345678, FileDispositionInfo, &disposition, sizeof(disposition)));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES);
	TEST_CHECK(DeleteFileA(name));
	make_name(name);
	file = open_file(name, GENERIC_READ | GENERIC_WRITE | DELETE);
	HANDLE other = open_file(name, GENERIC_READ);
	disposition.DeleteFile = TRUE;
	if (getenv("WIBO_FIXTURE_RUNTIME")) {
		TEST_CHECK(!update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
	} else {
		TEST_CHECK(update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
		disposition.DeleteFile = FALSE;
		TEST_CHECK(update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
	}
	TEST_CHECK(CloseHandle(other));
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES);
	TEST_CHECK(DeleteFileA(name));
	return 0;
}
