#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "child") == 0) {
		Sleep(8000);
		return 19;
	}

	HANDLE named = CreateJobObjectA(NULL, "wibo-fixture-job");
	TEST_CHECK(named != NULL);
	HANDLE existing = CreateJobObjectA(NULL, "wibo-fixture-job");
	TEST_CHECK(existing != NULL);
	TEST_CHECK_EQ(ERROR_ALREADY_EXISTS, GetLastError());
	TEST_CHECK(CloseHandle(existing));
	TEST_CHECK(CloseHandle(named));

	HANDLE job = CreateJobObjectA(NULL, NULL);
	TEST_CHECK(job != NULL);
	BOOL member = TRUE;
	TEST_CHECK(IsProcessInJob(GetCurrentProcess(), job, &member));
	TEST_CHECK_EQ(FALSE, member);

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	ZeroMemory(&limits, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	TEST_CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));

	char image[MAX_PATH];
	DWORD imageLength = GetModuleFileNameA(NULL, image, sizeof(image));
	TEST_CHECK(imageLength > 0 && imageLength < sizeof(image));
	char commandLine[MAX_PATH + 32];
	snprintf(commandLine, sizeof(commandLine), "\"%s\" child", image);
	STARTUPINFOA startup;
	PROCESS_INFORMATION process;
	ZeroMemory(&startup, sizeof(startup));
	ZeroMemory(&process, sizeof(process));
	startup.cb = sizeof(startup);
	TEST_CHECK(CreateProcessA(image, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
	TEST_CHECK(AssignProcessToJobObject(job, process.hProcess));
	TEST_CHECK(IsProcessInJob(process.hProcess, job, &member));
	TEST_CHECK_EQ(TRUE, member);
	TEST_CHECK(IsProcessInJob(process.hProcess, NULL, &member));
	TEST_CHECK_EQ(TRUE, member);

	HANDLE duplicate;
	TEST_CHECK(
		DuplicateHandle(GetCurrentProcess(), job, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS));
	TEST_CHECK(CloseHandle(job));
	TEST_CHECK_EQ(WAIT_TIMEOUT, WaitForSingleObject(process.hProcess, 0));
	TEST_CHECK(CloseHandle(duplicate));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(process.hProcess, 5000));
	TEST_CHECK(CloseHandle(process.hThread));
	TEST_CHECK(CloseHandle(process.hProcess));
	return 0;
}
