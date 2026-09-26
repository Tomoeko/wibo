#include "test_assert.h"
#include <windows.h>

static void check_nonconsole(HANDLE handle) {
	DWORD mode = 0x12345678;
	SetLastError(0x71);
	TEST_CHECK(!GetConsoleMode(handle, &mode));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK_EQ(0x12345678, mode);
	SetLastError(0x71);
	TEST_CHECK(!SetConsoleMode(handle, ENABLE_LINE_INPUT));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
}

int main(void) {
	HANDLE readPipe, writePipe;
	TEST_CHECK(CreatePipe(&readPipe, &writePipe, NULL, 0));
	check_nonconsole(readPipe);
	check_nonconsole(writePipe);
	HANDLE original = GetStdHandle(STD_INPUT_HANDLE);
	TEST_CHECK(SetStdHandle(STD_INPUT_HANDLE, readPipe));
	check_nonconsole(GetStdHandle(STD_INPUT_HANDLE));
	HANDLE duplicate;
	TEST_CHECK(DuplicateHandle(GetCurrentProcess(), readPipe, GetCurrentProcess(), &duplicate, 0, FALSE,
							   DUPLICATE_SAME_ACCESS));
	check_nonconsole(duplicate);
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK(SetStdHandle(STD_INPUT_HANDLE, original));
	TEST_CHECK(CloseHandle(readPipe));
	TEST_CHECK(CloseHandle(writePipe));
	const char *name = "wibo_console_redirect_fixture.tmp";
	HANDLE file = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
							  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	check_nonconsole(file);
	TEST_CHECK(CloseHandle(file));
	TEST_CHECK(DeleteFileA(name));
	HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(event != NULL);
	check_nonconsole(event);
	TEST_CHECK(CloseHandle(event));
	check_nonconsole(NULL);
	check_nonconsole(INVALID_HANDLE_VALUE);
	return 0;
}
