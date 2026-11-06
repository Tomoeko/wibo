#define _WIN32_WINNT 0x0601
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef BOOL(WINAPI *GroupAffinityFn)(HANDLE process, USHORT *count, USHORT *groups);

static void check_process(GroupAffinityFn query, HANDLE process) {
	USHORT count = 0;
	SetLastError(0x7315);
	TEST_CHECK(!query(process, &count, NULL));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
	TEST_CHECK_EQ(1, count);

	USHORT groups[4] = {0xaaaa, 0xbbbb, 0xcccc, 0xdddd};
	count = 4;
	SetLastError(0x7315);
	TEST_CHECK(query(process, &count, groups));
	TEST_CHECK_EQ(1, count);
	TEST_CHECK_EQ(0, groups[0]);
	TEST_CHECK_EQ(0xbbbb, groups[1]);
	TEST_CHECK_EQ(0xcccc, groups[2]);
	TEST_CHECK_EQ(0xdddd, groups[3]);
	TEST_CHECK_EQ(0x7315, GetLastError());

	count = 1;
	groups[0] = 0xaaaa;
	TEST_CHECK(query(process, &count, groups));
	TEST_CHECK_EQ(1, count);
	TEST_CHECK_EQ(0, groups[0]);
}

int main(void) {
	HMODULE kernel = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(kernel != NULL);
	FARPROC procedure = GetProcAddress(kernel, "GetProcessGroupAffinity");
	TEST_CHECK(procedure != NULL);
	GroupAffinityFn query;
	TEST_CHECK(sizeof(query) == sizeof(procedure));
	memcpy(&query, &procedure, sizeof(query));
	USHORT initialCount = 0;
	SetLastError(0x7315);
	if (!query(GetCurrentProcess(), &initialCount, NULL) && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED) {
		fprintf(stderr, "SKIP: process-group query is unimplemented in this runtime\n");
		return 77;
	}
	check_process(query, GetCurrentProcess());
	for (unsigned index = 0; index < 2; ++index) {
		const DWORD access = index == 0 ? PROCESS_QUERY_INFORMATION : PROCESS_QUERY_LIMITED_INFORMATION;
		HANDLE process = OpenProcess(access, FALSE, GetCurrentProcessId());
		TEST_CHECK(process != NULL);
		check_process(query, process);
		TEST_CHECK(CloseHandle(process));
	}
	HANDLE denied = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
	TEST_CHECK(denied != NULL);
	USHORT count = 1;
	USHORT group = 0xaaaa;
	SetLastError(0x7315);
	TEST_CHECK(!query(denied, &count, &group));
	TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetLastError());
	TEST_CHECK_EQ(1, count);
	TEST_CHECK_EQ(0xaaaa, group);
	TEST_CHECK(CloseHandle(denied));
	SetLastError(0x7315);
	TEST_CHECK(!query((HANDLE)(ULONG_PTR)0x12345678, &count, &group));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, GetLastError());
	TEST_CHECK_EQ(1, count);
	TEST_CHECK_EQ(0xaaaa, group);
	return 0;
}
