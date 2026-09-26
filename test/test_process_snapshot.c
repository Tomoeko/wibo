#include "test_assert.h"
#include <windows.h>

#include <tlhelp32.h>

int main(int argc, char **argv) {
	(void)argv;
	if (argc > 1) {
		Sleep(10000);
		return 0;
	}
	char path[4096], command[4200];
	TEST_CHECK(GetModuleFileNameA(NULL, path, sizeof(path)) > 0);
	const char *name = strrchr(path, '\\');
	name = name ? name + 1 : path;
	snprintf(command, sizeof(command), "\"%s\" child", path);
	STARTUPINFOA startup = {0};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process;
	TEST_CHECK(CreateProcessA(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_INHERIT, 0xffffffff);
	TEST_CHECK(snapshot != INVALID_HANDLE_VALUE);
	DWORD flags;
	TEST_CHECK(GetHandleInformation(snapshot, &flags));
	TEST_CHECK(flags & HANDLE_FLAG_INHERIT);
	TEST_CHECK(TerminateProcess(process.hProcess, 37));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 5000));
	PROCESSENTRY32 entry = {0};
	entry.dwSize = sizeof(entry) - 1;
	TEST_CHECK(!Process32First(snapshot, &entry));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	entry.dwSize = sizeof(entry);
	SetLastError(0x71);
	TEST_CHECK(Process32First(snapshot, &entry));
	TEST_CHECK_EQ(0x71, GetLastError());
	DWORD first = entry.th32ProcessID;
	BOOL self = FALSE, child = FALSE;
	do {
		if (entry.th32ProcessID == GetCurrentProcessId() || entry.th32ProcessID == process.dwProcessId) {
			TEST_CHECK_EQ(0, entry.cntUsage);
			TEST_CHECK_U64_EQ(0, entry.th32DefaultHeapID);
			TEST_CHECK_EQ(0, entry.th32ModuleID);
			TEST_CHECK_EQ(0, entry.dwFlags);
			TEST_CHECK(entry.cntThreads > 0);
			TEST_CHECK_STR_EQ(name, entry.szExeFile);
			if (entry.th32ProcessID == GetCurrentProcessId())
				self = TRUE;
			else {
				child = TRUE;
				TEST_CHECK_EQ(GetCurrentProcessId(), entry.th32ParentProcessID);
			}
		}
	} while (Process32Next(snapshot, &entry));
	TEST_CHECK_EQ(ERROR_NO_MORE_FILES, GetLastError());
	TEST_CHECK(self && child);
	TEST_CHECK(!Process32Next(snapshot, &entry));
	TEST_CHECK_EQ(ERROR_NO_MORE_FILES, GetLastError());
	PROCESSENTRY32W wide = {0};
	wide.dwSize = sizeof(wide);
	TEST_CHECK(Process32FirstW(snapshot, &wide));
	TEST_CHECK_EQ(first, wide.th32ProcessID);
	self = FALSE;
	do {
		if (wide.th32ProcessID == GetCurrentProcessId()) {
			WCHAR expected[260];
			TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, name, -1, expected, 260) > 0);
			TEST_CHECK(wcscmp(expected, wide.szExeFile) == 0);
			self = TRUE;
		}
	} while (Process32NextW(snapshot, &wide));
	TEST_CHECK(self);
	TEST_CHECK_EQ(ERROR_NO_MORE_FILES, GetLastError());
	TEST_CHECK(CloseHandle(snapshot));
	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	TEST_CHECK(snapshot != INVALID_HANDLE_VALUE);
	TEST_CHECK(!Process32First(snapshot, &entry));
	TEST_CHECK_EQ(ERROR_NO_MORE_FILES, GetLastError());
	TEST_CHECK(CloseHandle(snapshot));
	TEST_CHECK(!Process32First(INVALID_HANDLE_VALUE, &entry));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(process.hThread));
	TEST_CHECK(CloseHandle(process.hProcess));
	return 0;
}
