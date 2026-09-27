#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef BOOL(WINAPI *ConversionFunction)(const TIME_ZONE_INFORMATION *, const SYSTEMTIME *, LPSYSTEMTIME);

enum ZoneId {
	ZERO_OFFSET,
	POSITIVE_OFFSET,
	NEGATIVE_OFFSET,
	UNUSED_BIASES,
	NEXT_DAY_OFFSET,
	PREVIOUS_DAY_OFFSET,
	UNDERFLOW_OFFSET,
	NORTHERN_RULES,
	STANDARD_EXTRA_BIAS,
	HALF_HOUR_DAYLIGHT,
	ABSOLUTE_RULES,
	SOUTHERN_RULES,
	LAST_WEEKDAY,
	INVALID_WEEK,
	MISSING_STANDARD,
	IGNORED_NAMES
};

#define INPUT(y, m, d, h, n, s, ms) {y, m, 0, d, h, n, s, ms}
#define OUTPUT(y, m, w, d, h, n, s, ms) {y, m, w, d, h, n, s, ms}

typedef struct {
	const char *name;
	enum ZoneId zone;
	SYSTEMTIME local;
	BOOL succeeds;
	SYSTEMTIME expected;
} ConversionCase;

static const ConversionCase cases[] = {
	{"fixed-zero", ZERO_OFFSET, INPUT(2024, 2, 29, 12, 34, 56, 789), TRUE, OUTPUT(2024, 2, 4, 29, 12, 34, 56, 789)},
	{"fixed-positive", POSITIVE_OFFSET, INPUT(2024, 2, 29, 12, 34, 56, 789), TRUE,
	 OUTPUT(2024, 2, 4, 29, 14, 4, 56, 789)},
	{"fixed-negative", NEGATIVE_OFFSET, INPUT(2024, 2, 29, 12, 34, 56, 789), TRUE,
	 OUTPUT(2024, 2, 4, 29, 7, 4, 56, 789)},
	{"unused-biases", UNUSED_BIASES, INPUT(2024, 2, 29, 12, 34, 56, 789), TRUE, OUTPUT(2024, 2, 4, 29, 7, 4, 56, 789)},
	{"next-year", NEXT_DAY_OFFSET, INPUT(2024, 12, 31, 23, 34, 56, 789), TRUE, OUTPUT(2025, 1, 3, 1, 1, 34, 56, 789)},
	{"previous-year", PREVIOUS_DAY_OFFSET, INPUT(2024, 1, 1, 0, 15, 0, 0), TRUE, OUTPUT(2023, 12, 0, 31, 22, 15, 0, 0)},
	{"utc-underflow", UNDERFLOW_OFFSET, INPUT(1601, 1, 1, 0, 0, 0, 0), FALSE, {0}},
	{"north-winter", NORTHERN_RULES, INPUT(2024, 1, 15, 12, 34, 56, 789), TRUE,
	 OUTPUT(2024, 1, 1, 15, 17, 34, 56, 789)},
	{"north-summer", NORTHERN_RULES, INPUT(2024, 7, 15, 12, 34, 56, 789), TRUE,
	 OUTPUT(2024, 7, 1, 15, 16, 34, 56, 789)},
	{"spring-before", NORTHERN_RULES, INPUT(2024, 3, 10, 1, 59, 59, 999), TRUE, OUTPUT(2024, 3, 0, 10, 6, 59, 59, 999)},
	{"spring-gap-start", NORTHERN_RULES, INPUT(2024, 3, 10, 2, 0, 0, 0), TRUE, OUTPUT(2024, 3, 0, 10, 6, 0, 0, 0)},
	{"spring-gap-middle", NORTHERN_RULES, INPUT(2024, 3, 10, 2, 30, 0, 0), TRUE, OUTPUT(2024, 3, 0, 10, 6, 30, 0, 0)},
	{"spring-after", NORTHERN_RULES, INPUT(2024, 3, 10, 3, 0, 0, 0), TRUE, OUTPUT(2024, 3, 0, 10, 7, 0, 0, 0)},
	{"fall-before", NORTHERN_RULES, INPUT(2024, 11, 3, 0, 59, 59, 999), TRUE, OUTPUT(2024, 11, 0, 3, 4, 59, 59, 999)},
	{"fall-ambiguous", NORTHERN_RULES, INPUT(2024, 11, 3, 1, 30, 0, 0), TRUE, OUTPUT(2024, 11, 0, 3, 5, 30, 0, 0)},
	{"fall-after", NORTHERN_RULES, INPUT(2024, 11, 3, 2, 0, 0, 0), TRUE, OUTPUT(2024, 11, 0, 3, 7, 0, 0, 0)},
	{"standard-extra-bias", STANDARD_EXTRA_BIAS, INPUT(2024, 11, 3, 2, 0, 0, 0), TRUE,
	 OUTPUT(2024, 11, 0, 3, 7, 30, 0, 0)},
	{"half-hour-gap", HALF_HOUR_DAYLIGHT, INPUT(2024, 3, 10, 2, 15, 0, 0), TRUE, OUTPUT(2024, 3, 0, 10, 6, 45, 0, 0)},
	{"absolute-rules", ABSOLUTE_RULES, INPUT(2024, 7, 15, 12, 0, 0, 0), TRUE, OUTPUT(2024, 7, 1, 15, 16, 0, 0, 0)},
	{"south-summer", SOUTHERN_RULES, INPUT(2024, 1, 15, 12, 0, 0, 0), TRUE, OUTPUT(2024, 1, 1, 15, 1, 0, 0, 0)},
	{"south-winter", SOUTHERN_RULES, INPUT(2024, 7, 15, 12, 0, 0, 0), TRUE, OUTPUT(2024, 7, 1, 15, 2, 0, 0, 0)},
	{"last-weekday", LAST_WEEKDAY, INPUT(2024, 3, 31, 2, 30, 0, 0), TRUE, OUTPUT(2024, 3, 0, 31, 6, 30, 0, 0)},
	{"invalid-week", INVALID_WEEK, INPUT(2024, 3, 31, 2, 30, 0, 0), FALSE, {0}},
	{"missing-standard", MISSING_STANDARD, INPUT(2024, 3, 31, 2, 30, 0, 0), FALSE, {0}},
	{"invalid-day", NORTHERN_RULES, INPUT(2024, 2, 30, 12, 0, 0, 0), FALSE, {0}},
	{"invalid-hour", NORTHERN_RULES, INPUT(2024, 2, 29, 24, 0, 0, 0), FALSE, {0}},
	{"invalid-milliseconds", NORTHERN_RULES, INPUT(2024, 2, 29, 12, 0, 0, 1000), FALSE, {0}},
	{"ignored-names-weekday", IGNORED_NAMES, OUTPUT(2024, 2, 65535, 29, 12, 34, 56, 789), TRUE,
	 OUTPUT(2024, 2, 4, 29, 17, 34, 56, 789)},
};

static TIME_ZONE_INFORMATION makeZone(enum ZoneId id) {
	TIME_ZONE_INFORMATION zone;
	memset(&zone, 0, sizeof(zone));
	if (id <= UNDERFLOW_OFFSET) {
		const LONG offsets[] = {0, 90, -330, -330, 120, -120, -60};
		zone.Bias = offsets[id];
		if (id == UNUSED_BIASES) {
			zone.StandardBias = 27;
			zone.DaylightBias = -91;
		}
		return zone;
	}
	zone.Bias = 300;
	zone.DaylightBias = -60;
	zone.DaylightDate = (SYSTEMTIME)OUTPUT(0, 3, 0, 2, 2, 0, 0, 0);
	zone.StandardDate = (SYSTEMTIME)OUTPUT(0, 11, 0, 1, 2, 0, 0, 0);
	switch (id) {
	case STANDARD_EXTRA_BIAS:
		zone.StandardBias = 30;
		break;
	case HALF_HOUR_DAYLIGHT:
		zone.DaylightBias = -30;
		break;
	case ABSOLUTE_RULES:
		zone.StandardDate.wYear = zone.DaylightDate.wYear = 2024;
		zone.StandardDate.wDay = 3;
		zone.DaylightDate.wDay = 10;
		break;
	case SOUTHERN_RULES:
		zone.Bias = -600;
		zone.DaylightDate.wMonth = 10;
		zone.DaylightDate.wDay = 1;
		zone.StandardDate.wMonth = 4;
		break;
	case LAST_WEEKDAY:
		zone.DaylightDate.wDay = 5;
		break;
	case INVALID_WEEK:
		zone.StandardDate.wDay = 0;
		break;
	case MISSING_STANDARD:
		zone.StandardDate.wMonth = 0;
		break;
	case IGNORED_NAMES:
		for (size_t index = 0; index < 32; ++index)
			zone.StandardName[index] = zone.DaylightName[index] = 0xd801;
		break;
	default:
		break;
	}
	return zone;
}

static void checkConversion(ConversionFunction convert, const ConversionCase *test, BOOL inplace, DWORD failure) {
	TIME_ZONE_INFORMATION zone = makeZone(test->zone), zoneBefore = zone;
	const SYSTEMTIME localBefore = test->local;
	struct {
		WORD before[4];
		SYSTEMTIME value;
		WORD after[4];
	} output;
	memset(&output, 0xa5, sizeof(output));
	if (inplace)
		output.value = test->local;
	const SYSTEMTIME initial = output.value;
	SetLastError(0x4321);
	const BOOL result = convert(&zone, inplace ? &output.value : &test->local, &output.value);
	const DWORD error = GetLastError();
	TEST_CHECK_EQ(failure ? FALSE : test->succeeds, result != FALSE);
	TEST_CHECK_EQ(failure ? failure : test->succeeds ? 0x4321 : ERROR_INVALID_PARAMETER, error);
	TEST_CHECK(memcmp(&output.value, failure || !test->succeeds ? &initial : &test->expected, sizeof(initial)) == 0);
	TEST_CHECK(memcmp(&zone, &zoneBefore, sizeof(zone)) == 0);
	TEST_CHECK(memcmp(&test->local, &localBefore, sizeof(localBefore)) == 0);
	for (size_t index = 0; index < 4; ++index) {
		TEST_CHECK_EQ(0xa5a5, output.before[index]);
		TEST_CHECK_EQ(0xa5a5, output.after[index]);
	}
}

int main(void) {
	HMODULE module = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC procedure = GetProcAddress(module, "TzSpecificLocalTimeToSystemTime");
	ConversionFunction convert;
	TEST_CHECK(procedure != NULL && sizeof(procedure) == sizeof(convert));
	memcpy(&convert, &procedure, sizeof(convert));
	char mode[32] = {0};
	const DWORD modeLength = GetEnvironmentVariableA("WIBO_FIXTURE_TIMEZONE_REVERSE_RESPONSE", mode, sizeof(mode));
	TEST_CHECK(modeLength < sizeof(mode));
	if (modeLength && strcmp(mode, "success") != 0) {
		const DWORD failure = strcmp(mode, "failed") == 0		 ? ERROR_ACCESS_DENIED
							  : strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
																 : ERROR_INVALID_DATA;
		checkConversion(convert, &cases[1], FALSE, failure);
		if (strcmp(mode, "unavailable") == 0)
			return 0;
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_TIMEZONE_REVERSE_RESPONSE", "success"));
		checkConversion(convert, &cases[1], TRUE, 0);
		return 0;
	}
	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		checkConversion(convert, &cases[index], FALSE, 0);
		printf("case=%s verified\n", cases[index].name);
	}
	checkConversion(convert, &cases[1], TRUE, 0);
	TIME_ZONE_INFORMATION current;
	TEST_CHECK(GetTimeZoneInformation(&current) != TIME_ZONE_ID_INVALID);
	const SYSTEMTIME local = OUTPUT(2024, 2, 65535, 29, 12, 34, 56, 789);
	SYSTEMTIME explicitOutput, implicitOutput;
	memset(&explicitOutput, 0xa5, sizeof(explicitOutput));
	memset(&implicitOutput, 0xa5, sizeof(implicitOutput));
	SetLastError(0x4321);
	TEST_CHECK(convert(&current, &local, &explicitOutput));
	TEST_CHECK_EQ(0x4321, GetLastError());
	SetLastError(0x4321);
	TEST_CHECK(convert(NULL, &local, &implicitOutput));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK(memcmp(&explicitOutput, &implicitOutput, sizeof(explicitOutput)) == 0);
	puts("Reverse timezone fixture complete");
	return 0;
}
