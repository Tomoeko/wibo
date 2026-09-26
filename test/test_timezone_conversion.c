#include "test_assert.h"
#include <windows.h>

static SYSTEMTIME date(WORD year, WORD month, WORD day, WORD hour, WORD minute) {
	SYSTEMTIME value = {0};
	value.wYear = year;
	value.wMonth = month;
	value.wDay = day;
	value.wHour = hour;
	value.wMinute = minute;
	value.wSecond = 17;
	value.wMilliseconds = 123;
	return value;
}

static void check(const TIME_ZONE_INFORMATION *zone, SYSTEMTIME utc, SYSTEMTIME expected) {
	SYSTEMTIME output;
	memset(&output, 0xa5, sizeof(output));
	SetLastError(0x1234);
	TEST_CHECK(SystemTimeToTzSpecificLocalTime(zone, &utc, &output));
	TEST_CHECK_EQ(0x1234, GetLastError());
	TEST_CHECK_EQ(expected.wYear, output.wYear);
	TEST_CHECK_EQ(expected.wMonth, output.wMonth);
	TEST_CHECK_EQ(expected.wDay, output.wDay);
	TEST_CHECK_EQ(expected.wHour, output.wHour);
	TEST_CHECK_EQ(expected.wMinute, output.wMinute);
	TEST_CHECK_EQ(expected.wSecond, output.wSecond);
	TEST_CHECK_EQ(expected.wMilliseconds, output.wMilliseconds);
	FILETIME ticks;
	SYSTEMTIME normalized;
	TEST_CHECK(SystemTimeToFileTime(&expected, &ticks));
	TEST_CHECK(FileTimeToSystemTime(&ticks, &normalized));
	TEST_CHECK_EQ(normalized.wDayOfWeek, output.wDayOfWeek);
	TEST_CHECK(SystemTimeToTzSpecificLocalTime(zone, &utc, &utc));
	TEST_CHECK(memcmp(&utc, &output, sizeof(utc)) == 0);
}

static TIME_ZONE_INFORMATION northern_zone(void) {
	TIME_ZONE_INFORMATION zone = {0};
	zone.Bias = 300;
	zone.DaylightBias = -60;
	zone.DaylightDate.wMonth = 3;
	zone.DaylightDate.wDay = 2;
	zone.DaylightDate.wHour = 2;
	zone.StandardDate.wMonth = 11;
	zone.StandardDate.wDay = 1;
	zone.StandardDate.wHour = 2;
	return zone;
}

int main(void) {
	const char *fault = getenv("WIBO_FIXTURE_ZONE_RESPONSE");
	if (fault) {
		TIME_ZONE_INFORMATION current, untouched;
		memset(&current, 0xa5, sizeof(current));
		untouched = current;
		const DWORD expected = strcmp(fault, "failed") == 0 ? ERROR_ACCESS_DENIED : ERROR_INVALID_DATA;
		TEST_CHECK_EQ(TIME_ZONE_ID_INVALID, GetTimeZoneInformation(&current));
		TEST_CHECK_EQ(expected, GetLastError());
		TEST_CHECK(memcmp(&current, &untouched, sizeof(current)) == 0);
		SYSTEMTIME utc = date(2021, 7, 15, 12, 0), output, previous;
		memset(&output, 0xa5, sizeof(output));
		previous = output;
		TEST_CHECK(!SystemTimeToTzSpecificLocalTime(NULL, &utc, &output));
		TEST_CHECK_EQ(expected, GetLastError());
		TEST_CHECK(memcmp(&output, &previous, sizeof(output)) == 0);
		return EXIT_SUCCESS;
	}
	TIME_ZONE_INFORMATION zone = {0};
	zone.Bias = 90;
	zone.StandardBias = 10;
	zone.DaylightBias = -60;
	check(&zone, date(2021, 6, 15, 12, 0), date(2021, 6, 15, 10, 30));
	zone.Bias = -150;
	check(&zone, date(2021, 12, 31, 23, 0), date(2022, 1, 1, 1, 30));
	zone = northern_zone();
	check(&zone, date(2021, 1, 15, 12, 0), date(2021, 1, 15, 7, 0));
	check(&zone, date(2021, 7, 15, 12, 0), date(2021, 7, 15, 8, 0));
	check(&zone, date(2021, 3, 14, 6, 59), date(2021, 3, 14, 1, 59));
	check(&zone, date(2021, 3, 14, 7, 0), date(2021, 3, 14, 3, 0));
	check(&zone, date(2021, 11, 7, 5, 59), date(2021, 11, 7, 1, 59));
	check(&zone, date(2021, 11, 7, 6, 0), date(2021, 11, 7, 1, 0));
	zone.DaylightDate.wYear = zone.StandardDate.wYear = 2021;
	zone.DaylightDate.wDay = 14;
	zone.StandardDate.wDay = 7;
	check(&zone, date(2021, 7, 15, 12, 0), date(2021, 7, 15, 8, 0));
	zone = northern_zone();
	zone.Bias = -600;
	zone.StandardDate.wMonth = 4;
	zone.StandardDate.wHour = 3;
	zone.DaylightDate.wMonth = 10;
	zone.DaylightDate.wDay = 1;
	check(&zone, date(2021, 1, 15, 12, 0), date(2021, 1, 15, 23, 0));
	check(&zone, date(2021, 7, 15, 12, 0), date(2021, 7, 15, 22, 0));
	check(&zone, date(2021, 4, 3, 15, 59), date(2021, 4, 4, 2, 59));
	check(&zone, date(2021, 4, 3, 16, 0), date(2021, 4, 4, 2, 0));
	check(&zone, date(2021, 10, 2, 15, 59), date(2021, 10, 3, 1, 59));
	check(&zone, date(2021, 10, 2, 16, 0), date(2021, 10, 3, 3, 0));
	zone.Bias = 60;
	zone.StandardBias = 10;
	zone.DaylightBias = -20;
	zone.DaylightDate.wMonth = 1;
	zone.DaylightDate.wDay = 5;
	zone.StandardDate.wMonth = 11;
	zone.StandardDate.wDay = 5;
	zone.StandardDate.wDayOfWeek = 4;
	zone.StandardDate.wHour = 2;
	check(&zone, date(2021, 1, 31, 3, 9), date(2021, 1, 31, 1, 59));
	check(&zone, date(2021, 1, 31, 3, 10), date(2021, 1, 31, 2, 30));
	check(&zone, date(2021, 11, 25, 2, 39), date(2021, 11, 25, 1, 59));
	check(&zone, date(2021, 11, 25, 2, 40), date(2021, 11, 25, 1, 30));
	SYSTEMTIME utc = date(1601, 1, 1, 0, 0), output;
	zone = (TIME_ZONE_INFORMATION){0};
	zone.Bias = 1;
	memset(&output, 0xa5, sizeof(output));
	SYSTEMTIME untouched = output;
	TEST_CHECK(!SystemTimeToTzSpecificLocalTime(&zone, &utc, &output));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);
	utc = date(2021, 2, 30, 12, 0);
	TEST_CHECK(!SystemTimeToTzSpecificLocalTime(&zone, &utc, &output));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());
	TEST_CHECK(memcmp(&output, &untouched, sizeof(output)) == 0);
	TIME_ZONE_INFORMATION current;
	TEST_CHECK(GetTimeZoneInformation(&current) != TIME_ZONE_ID_INVALID);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(300, current.Bias);
		TEST_CHECK_EQ(-60, current.DaylightBias);
		TEST_CHECK_EQ(3, current.DaylightDate.wMonth);
		TEST_CHECK_EQ(11, current.StandardDate.wMonth);
		TEST_CHECK_EQ(0, current.DaylightDate.wYear);
	}

	for (unsigned month = 1; month <= 12; ++month) {
		utc = date(2021, (WORD)month, 15, 12, 0);
		SYSTEMTIME expected;
		if (getenv("WIBO_FIXTURE_NATIVE_ZONE")) {
			FILETIME ticks, local;
			TEST_CHECK(SystemTimeToFileTime(&utc, &ticks));
			TEST_CHECK(FileTimeToLocalFileTime(&ticks, &local));
			TEST_CHECK(FileTimeToSystemTime(&local, &expected));
		} else {
			TEST_CHECK(SystemTimeToTzSpecificLocalTime(&current, &utc, &expected));
		}
		check(NULL, utc, expected);
	}
	return EXIT_SUCCESS;
}
