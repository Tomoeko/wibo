#include "test_assert.h"
#include <string.h>
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

	DWORD count = 0x12345678;
	SetLastError(0x71);
	TEST_CHECK(!WriteConsoleW(handle, L"A\x00e9", 2, &count, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK_EQ(0, count);
	WCHAR text[8];
	memset(text, 0xa5, sizeof(text));
	count = 0x12345678;
	SetLastError(0x71);
	TEST_CHECK(!ReadConsoleW(handle, text, 3, &count, NULL));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK_EQ(0, count);
	for (size_t index = 0; index < sizeof(text) / sizeof(text[0]); ++index)
		TEST_CHECK_EQ(0xa5a5, text[index]);
	INPUT_RECORD events[2];
	memset(events, 0xa5, sizeof(events));
	count = 0x12345678;
	SetLastError(0x71);
	TEST_CHECK(!PeekConsoleInputA(handle, events, 2, &count));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK_EQ(0x12345678, count);
	SetLastError(0x71);
	TEST_CHECK(!ReadConsoleInputA(handle, events, 2, &count));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK_EQ(0x12345678, count);
	for (size_t index = 0; index < sizeof(events); ++index)
		TEST_CHECK_EQ(0xa5, ((BYTE *)events)[index]);
	CONSOLE_SCREEN_BUFFER_INFO info;
	memset(&info, 0xa5, sizeof(info));
	SetLastError(0x71);
	TEST_CHECK(!GetConsoleScreenBufferInfo(handle, &info));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	for (size_t index = 0; index < sizeof(info); ++index)
		TEST_CHECK_EQ(0xa5, ((BYTE *)&info)[index]);
	SetLastError(0x71);
	TEST_CHECK(!SetConsoleTextAttribute(handle, 7));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
}

static void check_read_validation(void) {
	WCHAR text[] = {1, 2, 3, 4};
	CONSOLE_READCONSOLE_CONTROL control = {sizeof(control), 0, 0, 0};
	for (unsigned index = 0; index < 5; ++index) {
		control.nLength = index == 1 ? 0 : sizeof(control);
		control.nInitialChars = index == 2 ? 3 : 0;
		DWORD count = 0x12345678;
		SetLastError(0x71);
		TEST_CHECK(!ReadConsoleW(NULL, text, index == 0 ? 0x80000000 : 3, &count, index == 4 ? NULL : &control));
		const DWORD expectedError = index == 0	? ERROR_NOT_ENOUGH_MEMORY
									: index < 3 ? ERROR_INVALID_PARAMETER
												: ERROR_INVALID_HANDLE;
		TEST_CHECK_EQ(expectedError, GetLastError());
		TEST_CHECK_EQ(index < 3 ? 0x12345678 : 0, count);
		for (unsigned unit = 0; unit < 4; ++unit)
			TEST_CHECK_EQ(unit + 1, text[unit]);
		TEST_CHECK_EQ(0, control.dwControlKeyState);
	}
}

int main(void) {
	check_read_validation();
	HANDLE readPipe, writePipe;
	TEST_CHECK(CreatePipe(&readPipe, &writePipe, NULL, 0));
	check_nonconsole(readPipe);
	check_nonconsole(writePipe);
	DWORD available = 0;
	TEST_CHECK(PeekNamedPipe(readPipe, NULL, 0, NULL, &available, NULL));
	TEST_CHECK_EQ(0, available);
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
