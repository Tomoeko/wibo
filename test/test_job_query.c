#include <windows.h>

#include "test_assert.h"

int main(void) {
	HANDLE job = CreateJobObjectW(NULL, NULL);
	TEST_CHECK(job != NULL);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	DWORD returned = 0;
	ZeroMemory(&limits, sizeof(limits));
	TEST_CHECK(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), &returned));
	TEST_CHECK_EQ(sizeof(limits), returned);
	TEST_CHECK_EQ(0, limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);

	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	TEST_CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
	ZeroMemory(&limits, sizeof(limits));
	TEST_CHECK(QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), &returned));
	TEST_CHECK_EQ(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
				  limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);

	JOBOBJECT_BASIC_LIMIT_INFORMATION basic;
	ZeroMemory(&basic, sizeof(basic));
	TEST_CHECK(QueryInformationJobObject(job, JobObjectBasicLimitInformation, &basic, sizeof(basic), &returned));
	TEST_CHECK_EQ(sizeof(basic), returned);
	TEST_CHECK_EQ(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, basic.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);

	TEST_CHECK(!QueryInformationJobObject(INVALID_HANDLE_VALUE, JobObjectExtendedLimitInformation, &limits,
										  sizeof(limits), &returned));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK(CloseHandle(job));
	return 0;
}
