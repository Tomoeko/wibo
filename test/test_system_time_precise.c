#include <stdint.h>
#include <windows.h>

#include "test_assert.h"

typedef VOID(WINAPI *precise_time_fn)(LPFILETIME);

typedef struct {
	BYTE before[8];
	FILETIME value;
	BYTE after[8];
} GuardedTime;

static ULONGLONG ticks(FILETIME value) { return ((ULONGLONG)value.dwHighDateTime << 32) | value.dwLowDateTime; }

int main(void) {
	HMODULE module = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC symbol = GetProcAddress(module, "GetSystemTimePreciseAsFileTime");
	precise_time_fn precise = NULL;
	TEST_CHECK(sizeof(precise) == sizeof(symbol));
	memcpy(&precise, &symbol, sizeof(precise));
	TEST_CHECK(precise != NULL);
	ULONGLONG previous = 0, minimumStep = ~(ULONGLONG)0;
	unsigned distinct = 0, backward = 0;
	for (unsigned iteration = 0; iteration < 256; ++iteration) {
		GuardedTime guarded;
		FILETIME before, after;
		memset(&guarded, 0xa5, sizeof(guarded));
		GetSystemTimeAsFileTime(&before);
		SetLastError(0x4321);
		precise(&guarded.value);
		DWORD error = GetLastError();
		GetSystemTimeAsFileTime(&after);
		TEST_CHECK_EQ(0x4321, error);
		for (unsigned index = 0; index < sizeof(guarded.before); ++index) {
			TEST_CHECK_EQ(0xa5, guarded.before[index]);
			TEST_CHECK_EQ(0xa5, guarded.after[index]);
		}
		ULONGLONG sample = ticks(guarded.value);
		ULONGLONG lower = ticks(before), upper = ticks(after);
		const ULONGLONG tolerance = 2 * 10000000ULL;
		TEST_CHECK(sample > 116444736000000000ULL);
		TEST_CHECK(sample >= (lower > tolerance ? lower - tolerance : 0));
		TEST_CHECK(sample <= upper + tolerance);
		if (iteration != 0 && sample != previous)
			++distinct;
		if (iteration != 0 && sample < previous)
			++backward;
		if (iteration != 0 && sample > previous && sample - previous < minimumStep)
			minimumStep = sample - previous;
		previous = sample;
	}
	printf("samples=256 distinct=%u backward=%u minimumPositiveStep100ns=%llu\n", distinct, backward,
		   (unsigned long long)(minimumStep == ~(ULONGLONG)0 ? 0 : minimumStep));
	return 0;
}
