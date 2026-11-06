#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "test_assert.h"

int main(int argc, char **argv) {
	TEST_CHECK_EQ(2, argc);
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC procedure = GetProcAddress(kernel, "SetFileInformationByHandle");
	TEST_CHECK(procedure != NULL);
	typedef BOOL(WINAPI * SetInformationFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, void *, DWORD);
	SetInformationFn update;
	TEST_CHECK(sizeof(update) == sizeof(procedure));
	memcpy(&update, &procedure, sizeof(update));
	HANDLE file = CreateFileA(argv[1], GENERIC_READ | GENERIC_WRITE | DELETE,
							  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
							  FILE_ATTRIBUTE_NORMAL, NULL);
	TEST_CHECK(file != INVALID_HANDLE_VALUE);
	FILE_DISPOSITION_INFO disposition = {TRUE};
	TEST_CHECK(update(file, FileDispositionInfo, &disposition, sizeof(disposition)));
	TEST_CHECK(fputs("ready\n", stdout) >= 0);
	TEST_CHECK_EQ(0, fflush(stdout));
	TEST_CHECK_EQ('\n', getchar());
	TEST_CHECK(CloseHandle(file));
	return 0;
}
