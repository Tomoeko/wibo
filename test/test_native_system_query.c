#include "test_assert.h"
#include <stdlib.h>
#include <windows.h>
#include <winternl.h>
typedef NTSTATUS(WINAPI *Query)(ULONG, PVOID, ULONG, PULONG);
static Query query;
typedef struct {
	LONGLONG boot, current, bias;
	ULONG zone, reserved;
	ULONGLONG bootBias, sleepBias;
} Clock;
typedef struct {
	LONGLONG idle, kernel, user, dpc, interrupt;
	ULONG count, reserved;
} Cpu;
static void untouched(const BYTE *data, unsigned start, unsigned size) {
	for (unsigned i = start; i < size; ++i)
		TEST_CHECK_EQ(0x61, data[i]);
}
int main(void) {
	query = (Query)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
	TEST_CHECK(query != NULL);
	Clock clock;
	ULONG used;
	SetLastError(0x51);
	TEST_CHECK_EQ(0, query(3, &clock, sizeof(clock), &used));
	TEST_CHECK_EQ(sizeof(clock), used);
	TEST_CHECK_EQ(0x51, GetLastError());
	TEST_CHECK(clock.boot > 0 && clock.current >= clock.boot);
	FILETIME time;
	GetSystemTimeAsFileTime(&time);
	ULARGE_INTEGER now;
	now.LowPart = time.dwLowDateTime;
	now.HighPart = time.dwHighDateTime;
	LONGLONG delta = (LONGLONG)now.QuadPart - clock.current;
	TEST_CHECK(delta > -100000000LL && delta < 100000000LL);
	BYTE data[4096];
	const ULONG lengths[] = {0, 1, 24, 47, 48};
	for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
		const ULONG length = lengths[i];
		memset(data, 0x61, sizeof(data));
		used = 0x71717171;
		SetLastError(0x51);
		TEST_CHECK_EQ(0, query(3, data, length, &used));
		TEST_CHECK_EQ(length, used);
		TEST_CHECK_EQ(0x51, GetLastError());
		untouched(data, length, sizeof(data));
	}
	memset(data, 0x61, sizeof(data));
	used = 0x71717171;
	TEST_CHECK_U64_EQ(0xc0000004U, (ULONG)query(3, data, 49, &used));
	TEST_CHECK_EQ(0, used);
	untouched(data, 0, sizeof(data));
	memset(data, 0x61, sizeof(data));
	SetLastError(0x51);
	TEST_CHECK_EQ(0, query(8, data, sizeof(data), &used));
	TEST_CHECK_EQ(0x51, GetLastError());
	TEST_CHECK(used > 0 && used <= sizeof(data) && used % sizeof(Cpu) == 0);
	for (unsigned i = 0; i < used / sizeof(Cpu); ++i) {
		Cpu cpu;
		memcpy(&cpu, data + i * sizeof(cpu), sizeof(cpu));
		TEST_CHECK(cpu.idle >= 0 && cpu.kernel >= cpu.idle && cpu.user >= 0);
	}
	untouched(data, used, sizeof(data));
	Cpu cpu;
	TEST_CHECK_EQ(0, query(8, &cpu, sizeof(cpu), &used));
	TEST_CHECK_EQ(sizeof(cpu), used);
	TEST_CHECK_EQ(0, query(8, &cpu, sizeof(cpu), NULL));
	memset(data, 0x61, sizeof(data));
	used = 0x71717171;
	TEST_CHECK_U64_EQ(0xc0000004U, (ULONG)query(8, data, 47, &used));
	TEST_CHECK_EQ(0, used);
	untouched(data, 0, sizeof(data));
	used = 0x71717171;
	TEST_CHECK_U64_EQ(0xc0000005U, (ULONG)query(3, NULL, 0, &used));
	TEST_CHECK_EQ(0, used);
	used = 0x71717171;
	TEST_CHECK_U64_EQ(0xc0000004U, (ULONG)query(8, NULL, 0, &used));
	TEST_CHECK_EQ(0, used);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(0x100000001LL, cpu.idle);
		const char *faults[] = {"truncated", "trailing", "oversized", "failed"};
		for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
			TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_SYSTEM_RESPONSE", faults[i]));
			memset(data, 0x61, sizeof(data));
			used = 0x71717171;
			TEST_CHECK_U64_EQ(i == 3 ? 0xc0000022U : 0xc00000e9U, (ULONG)query(8, data, sizeof(data), &used));
			TEST_CHECK_EQ(0x71717171, used);
			untouched(data, 0, sizeof(data));
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_SYSTEM_RESPONSE", NULL));
	}
	return 0;
}
