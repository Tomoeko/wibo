#include "test_assert.h"
#include <windows.h>

static int find_entry(const char *block, const char *name, const char *expected) {
	const size_t length = strlen(name);
	for (const char *entry = block; *entry; entry += strlen(entry) + 1)
		if (_strnicmp(entry, name, length) == 0 && entry[length] == '=' && strcmp(entry + length + 1, expected) == 0)
			return 1;
	return 0;
}

int main(int argc, char **argv) {
	(void)argv;
	if (argc > 1) {
		char unchanged[16] = "unchanged";
		TEST_CHECK_EQ(0, GetEnvironmentVariableA("APPDATA", unchanged, sizeof(unchanged)));
		TEST_CHECK_EQ(ERROR_ENVVAR_NOT_FOUND, GetLastError());
		TEST_CHECK(strcmp(unchanged, "unchanged") == 0);
		return 0;
	}
	char path[1024];
	DWORD length = GetEnvironmentVariableA("APPDATA", path, sizeof(path));
	TEST_CHECK(length > 0 && length < sizeof(path));
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK(strcmp(path, "C:\\Fixture\\Roaming") == 0);
		TEST_CHECK(GetEnvironmentVariableA("localappdata", path, sizeof(path)) > 0);
		TEST_CHECK(strcmp(path, "C:\\Fixture\\Override") == 0);
		LPCH block = GetEnvironmentStringsA();
		TEST_CHECK(block != NULL);
		TEST_CHECK(find_entry(block, "APPDATA", "C:\\Fixture\\Roaming"));
		TEST_CHECK(find_entry(block, "LOCALAPPDATA", "C:\\Fixture\\Override"));
		TEST_CHECK(FreeEnvironmentStringsA(block));
	}
	TEST_CHECK(SetEnvironmentVariableA("appdata", "C:\\Fixture\\Changed"));
	TEST_CHECK(GetEnvironmentVariableA("APPDATA", path, sizeof(path)) > 0);
	TEST_CHECK(strcmp(path, "C:\\Fixture\\Changed") == 0);
	TEST_CHECK(SetEnvironmentVariableA("AppData", NULL));
	TEST_CHECK_EQ(0, GetEnvironmentVariableA("APPDATA", path, sizeof(path)));
	TEST_CHECK_EQ(ERROR_ENVVAR_NOT_FOUND, GetLastError());
	LPCH block = GetEnvironmentStringsA();
	TEST_CHECK(block != NULL);
	TEST_CHECK(!find_entry(block, "APPDATA", "C:\\Fixture\\Roaming"));
	TEST_CHECK(FreeEnvironmentStringsA(block));
	char module[MAX_PATH], command[MAX_PATH + 32];
	TEST_CHECK(GetModuleFileNameA(NULL, module, sizeof(module)) > 0);
	snprintf(command, sizeof(command), "\"%s\" unavailable", module);
	STARTUPINFOA startup = {0};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION child = {0};
	TEST_CHECK(CreateProcessA(module, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(child.hProcess, 5000));
	DWORD exitCode = 0;
	TEST_CHECK(GetExitCodeProcess(child.hProcess, &exitCode));
	TEST_CHECK_EQ(0, exitCode);
	TEST_CHECK(CloseHandle(child.hThread));
	TEST_CHECK(CloseHandle(child.hProcess));
	return 0;
}
