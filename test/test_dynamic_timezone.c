#define _WIN32_WINNT 0x0600
#include <stddef.h>
#include <windows.h>

#include "test_assert.h"

int main(void) {
	TEST_CHECK_EQ(432, sizeof(DYNAMIC_TIME_ZONE_INFORMATION));
	TEST_CHECK_EQ(172, offsetof(DYNAMIC_TIME_ZONE_INFORMATION, TimeZoneKeyName));
	TEST_CHECK_EQ(428, offsetof(DYNAMIC_TIME_ZONE_INFORMATION, DynamicDaylightTimeDisabled));
	DYNAMIC_TIME_ZONE_INFORMATION zone, untouched;
	memset(&zone, 0xa5, sizeof(zone));
	untouched = zone;
	const char *fault = getenv("WIBO_FIXTURE_ZONE_RESPONSE");
	DWORD state = GetDynamicTimeZoneInformation(&zone);
	if (fault) {
		TEST_CHECK_EQ(TIME_ZONE_ID_INVALID, state);
		TEST_CHECK_EQ(strcmp(fault, "failed") == 0 ? ERROR_ACCESS_DENIED : ERROR_INVALID_DATA, GetLastError());
		TEST_CHECK(memcmp(&zone, &untouched, sizeof(zone)) == 0);
		return 0;
	}
	TEST_CHECK(state <= TIME_ZONE_ID_DAYLIGHT);
	TEST_CHECK(zone.DynamicDaylightTimeDisabled == FALSE || zone.DynamicDaylightTimeDisabled == TRUE);
	TEST_CHECK(zone.TimeZoneKeyName[127] == 0);
	TIME_ZONE_INFORMATION basic;
	TEST_CHECK_EQ(state, GetTimeZoneInformation(&basic));
	TEST_CHECK(memcmp(&zone, &basic, sizeof(basic)) == 0);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(TIME_ZONE_ID_DAYLIGHT, state);
		TEST_CHECK_EQ(300, zone.Bias);
		TEST_CHECK(wcscmp(zone.TimeZoneKeyName, L"Synthetic Zone") == 0);
		TEST_CHECK_EQ(FALSE, zone.DynamicDaylightTimeDisabled);
	}
	return 0;
}
