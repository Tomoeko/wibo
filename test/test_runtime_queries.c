#include "test_assert.h"

#include <stdlib.h>
#include <windows.h>

static ULONGLONG ticks(FILETIME value) { return ((ULONGLONG)value.dwHighDateTime << 32) | value.dwLowDateTime; }

int main(void) {
	TEST_CHECK_EQ(1, GetActiveProcessorGroupCount());

	FILETIME creation = {0}, exit_time = {0}, kernel = {0}, user = {0}, now = {0};
	TEST_CHECK(GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user));
	GetSystemTimeAsFileTime(&now);
	TEST_CHECK(ticks(creation) > 0);
	TEST_CHECK(ticks(creation) <= ticks(now));
	HANDLE limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
	TEST_CHECK(limited != NULL);
	TEST_CHECK(GetProcessTimes(limited, &creation, &exit_time, &kernel, &user));
	TEST_CHECK(CloseHandle(limited));

	LCID locale = GetSystemDefaultLCID();
	TEST_CHECK(locale != 0);
	TEST_CHECK_EQ(locale & 0xffff, GetSystemDefaultLangID());
	if (getenv("WIBO_FIXTURE_PROVIDER"))
		TEST_CHECK_EQ(0x0411, locale);

	WCHAR source[] = L"A";
	WORD types[3] = {0xffff, 0xffff, 0xffff};
	TEST_CHECK(GetStringTypeExW(locale, CT_CTYPE1, source, -1, types));
	TEST_CHECK(types[0] & C1_UPPER);
	TEST_CHECK(types[0] & C1_ALPHA);
	TEST_CHECK(types[1] != 0xffff);
	TEST_CHECK_EQ(0xffff, types[2]);

	SYSTEM_INFO system_info;
	GetSystemInfo(&system_info);
	TEST_CHECK(system_info.dwPageSize > 0);
	void *region = VirtualAlloc(NULL, system_info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(region != NULL);
	TEST_CHECK(VirtualLock(region, system_info.dwPageSize));
	TEST_CHECK(VirtualLock(region, system_info.dwPageSize));
	TEST_CHECK(VirtualUnlock(region, system_info.dwPageSize));
	TEST_CHECK(VirtualFree(region, 0, MEM_RELEASE));
	return 0;
}
