#include "test_assert.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include <shellapi.h>

static void join_path(WCHAR *output, const WCHAR *directory, const WCHAR *name) {
	const size_t left = wcslen(directory), right = wcslen(name);
	TEST_CHECK(left + right + 2 <= MAX_PATH);
	memcpy(output, directory, left * sizeof(*output));
	output[left] = L'\\';
	memcpy(output + left + 1, name, (right + 1) * sizeof(*output));
}

static void create_file(const WCHAR *path) {
	HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	TEST_CHECK(CloseHandle(file));
}

static void check_result(const WCHAR *file, const WCHAR *directory, uintptr_t expected_result, DWORD expected_error,
						 const WCHAR *expected_text) {
	struct {
		DWORD before[2];
		WCHAR text[MAX_PATH];
		DWORD after[2];
	} result;
	WCHAR cwd_before[MAX_PATH], cwd_after[MAX_PATH];
	TEST_CHECK(GetCurrentDirectoryW(MAX_PATH, cwd_before));
	for (unsigned i = 0; i < 2; ++i)
		result.before[i] = result.after[i] = 0x1234abcd;
	for (unsigned i = 0; i < MAX_PATH; ++i)
		result.text[i] = 0x6b6b;
	SetLastError(0x13579bdf);
	const HINSTANCE returned = FindExecutableW(file, directory, result.text);
	const DWORD error = GetLastError();
	TEST_CHECK_EQ(expected_result, (uintptr_t)returned);
	TEST_CHECK_EQ(expected_error, error);
	TEST_CHECK(wcscmp(expected_text, result.text) == 0);
	for (size_t i = wcslen(expected_text) + 1; i < MAX_PATH; ++i)
		TEST_CHECK_EQ(0x6b6b, result.text[i]);
	for (unsigned i = 0; i < 2; ++i) {
		TEST_CHECK_EQ(0x1234abcd, result.before[i]);
		TEST_CHECK_EQ(0x1234abcd, result.after[i]);
	}
	TEST_CHECK(GetCurrentDirectoryW(MAX_PATH, cwd_after));
	TEST_CHECK(wcscmp(cwd_before, cwd_after) == 0);
}

int main(int argc, char **argv) {
	const int unsupported = argc == 2 && strcmp(argv[1], "unsupported") == 0;
	TEST_CHECK(argc == 1 || unsupported);
	WCHAR temp[MAX_PATH], root[MAX_PATH], program[MAX_PATH], spaced[MAX_PATH], script[MAX_PATH];
	WCHAR document[MAX_PATH], missing_file[MAX_PATH], missing_dir[MAX_PATH], missing_path[MAX_PATH],
		no_extension[MAX_PATH];
	const DWORD temp_length = GetTempPathW(MAX_PATH, temp);
	TEST_CHECK(temp_length && temp_length < MAX_PATH);
	TEST_CHECK(GetTempFileNameW(temp, L"wfx", 0, root));
	TEST_CHECK(DeleteFileW(root));
	TEST_CHECK(CreateDirectoryW(root, NULL));
	join_path(program, root, L"fixture-program.exe");
	join_path(spaced, root, L"fixture program.exe");
	join_path(script, root, L"fixture-script.cmd");
	join_path(document, root, L"fixture-document.wfx");
	join_path(missing_file, root, L"missing-file.exe");
	join_path(missing_dir, root, L"missing-directory");
	join_path(missing_path, missing_dir, L"missing-file.exe");
	join_path(no_extension, root, L"fixture-program");
	create_file(program);
	create_file(spaced);
	create_file(script);
	create_file(document);
	if (unsupported) {
		check_result(document, NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(root, NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(L"fixture-program.exe", root, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(L"", NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(L"C:fixture-program.exe", NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(L"Q:\\fixture-program.exe", NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(L"\\\\server\\share\\fixture-program.exe", NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(L"\\\\?\\C:\\fixture-program.exe", NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(program, L"C:directory", 0, ERROR_NOT_SUPPORTED, L"");
		check_result(no_extension, NULL, 0, ERROR_NOT_SUPPORTED, L"");
		check_result(L".\\fixture-program.exe", root, 0, ERROR_NOT_SUPPORTED, L"");
	} else {
		check_result(NULL, NULL, SE_ERR_FNF, 0x13579bdf, L"");
		check_result(program, NULL, 33, 0x13579bdf, program);
		check_result(spaced, NULL, 33, 0x13579bdf, spaced);
		check_result(script, NULL, 33, 0x13579bdf, script);
		check_result(program, root, 33, 0x13579bdf, program);
		check_result(program, L"", 33, ERROR_INVALID_NAME, program);
		check_result(program, missing_dir, 33, ERROR_FILE_NOT_FOUND, program);
		check_result(missing_file, NULL, SE_ERR_FNF, ERROR_FILE_NOT_FOUND, L"");
		check_result(missing_path, NULL, SE_ERR_FNF, ERROR_PATH_NOT_FOUND, L"");
	}
	TEST_CHECK(DeleteFileW(program));
	TEST_CHECK(DeleteFileW(spaced));
	TEST_CHECK(DeleteFileW(script));
	TEST_CHECK(DeleteFileW(document));
	TEST_CHECK(RemoveDirectoryW(root));
	return 0;
}
