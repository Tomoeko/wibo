#include "test_assert.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

int main(void) {
	HANDLE file = CreateFileW(L"volume_by_handle.tmp", GENERIC_READ | GENERIC_WRITE,
							  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
							  FILE_FLAG_DELETE_ON_CLOSE, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	WCHAR label[128] = {0}, fileSystem[128] = {0};
	DWORD serial = 0, maximumComponent = 0, flags = 0;
	TEST_CHECK(GetVolumeInformationByHandleW(file, label, 128, &serial, &maximumComponent, &flags, fileSystem, 128));
	TEST_CHECK(maximumComponent > 0);
	TEST_CHECK(fileSystem[0] != L'\0');
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(0x12345678, serial);
		TEST_CHECK(wcscmp(label, L"Fixture") == 0);
		TEST_CHECK(wcscmp(fileSystem, L"FixtureFS") == 0);
	}
	TEST_CHECK(!GetVolumeInformationByHandleW(INVALID_HANDLE_VALUE, label, 128, &serial, &maximumComponent, &flags,
											  fileSystem, 128));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(file));
	return 0;
}
