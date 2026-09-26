#include "test_assert.h"
#include <windows.h>

static DWORD WINAPI query(void *result) {
	TIME_ZONE_INFORMATION zone;
	SetLastError(0x1234);
	TEST_CHECK_EQ(TIME_ZONE_ID_UNKNOWN, GetTimeZoneInformation(&zone));
	TEST_CHECK_EQ(0x1234, GetLastError());
	*(LONG *)result = zone.Bias;
	return 0;
}

int main(void) {
	const char *mode = getenv("WIBO_FIXTURE_STREAM_MODE");
	if (!mode) {
		TIME_ZONE_INFORMATION first, second;
		const DWORD state = GetTimeZoneInformation(&first);
		TEST_CHECK(state <= TIME_ZONE_ID_DAYLIGHT);
		TEST_CHECK_EQ(state, GetTimeZoneInformation(&second));
		TEST_CHECK(memcmp(&first, &second, sizeof(first)) == 0);
		return EXIT_SUCCESS;
	}

	if (mode && strcmp(mode, "fresh") != 0 && strcmp(mode, "concurrent") != 0) {
		TIME_ZONE_INFORMATION zone, previous;
		memset(&zone, 0xa5, sizeof(zone));
		previous = zone;
		TEST_CHECK_EQ(TIME_ZONE_ID_INVALID, GetTimeZoneInformation(&zone));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(memcmp(&zone, &previous, sizeof(zone)) == 0);
	}
	if (mode && strcmp(mode, "concurrent") == 0) {
		LONG values[16] = {0};
		HANDLE threads[16];
		for (unsigned index = 0; index < 16; ++index) {
			threads[index] = CreateThread(NULL, 0, query, &values[index], 0, NULL);
			TEST_CHECK(threads[index] != NULL);
		}
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(16, threads, TRUE, 10000));
		unsigned seen = 0;
		for (unsigned index = 0; index < 16; ++index) {
			TEST_CHECK(values[index] >= 101 && values[index] <= 116);
			unsigned bit = 1u << (values[index] - 101);
			TEST_CHECK(!(seen & bit));
			seen |= bit;
			TEST_CHECK(CloseHandle(threads[index]));
		}
		TEST_CHECK_EQ(0xffff, seen);
	} else {
		for (LONG expected = 101; expected <= 116; ++expected) {
			LONG result = 0;
			query(&result);
			TEST_CHECK_EQ(expected, result);
		}
	}
	return EXIT_SUCCESS;
}
