#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

typedef int(WINAPI *FormatFunction)(LCID, DWORD, const SYSTEMTIME *, LPCWSTR, LPWSTR, int);

static void loadProcedure(HMODULE module, const char *name, FormatFunction *destination) {
	FARPROC procedure = GetProcAddress(module, name);
	TEST_CHECK(procedure != NULL);
	TEST_CHECK_EQ(sizeof(procedure), sizeof(*destination));
	memcpy(destination, &procedure, sizeof(procedure));
}

static void checkFormat(FormatFunction function, LCID locale, DWORD flags, const SYSTEMTIME *value, LPCWSTR picture,
						int capacity, BOOL hasOutput, int expectedResult, DWORD expectedError, LPCWSTR written) {
	struct {
		WCHAR before[4];
		WCHAR output[64];
		WCHAR after[4];
	} guarded;
	TEST_CHECK(capacity <= 64);
	for (size_t index = 0; index < sizeof(guarded) / sizeof(WCHAR); ++index)
		((WCHAR *)&guarded)[index] = 0xa5a5;
	SetLastError(0x4321);
	TEST_CHECK_EQ(expectedResult, function(locale, flags, value, picture, hasOutput ? guarded.output : NULL, capacity));
	TEST_CHECK_EQ(expectedError, GetLastError());
	const size_t changed = written ? wcslen(written) + 1 : 0;
	TEST_CHECK(changed <= (size_t)(capacity > 0 ? capacity : 0));
	if (changed)
		TEST_CHECK(memcmp(guarded.output, written, changed * sizeof(WCHAR)) == 0);
	for (size_t index = changed; index < 64; ++index)
		TEST_CHECK_EQ(0xa5a5, guarded.output[index]);
	for (size_t index = 0; index < 4; ++index) {
		TEST_CHECK_EQ(0xa5a5, guarded.before[index]);
		TEST_CHECK_EQ(0xa5a5, guarded.after[index]);
	}
}

static void checkCommon(FormatFunction function, const SYSTEMTIME *value, LPCWSTR picture, LPCWSTR expected) {
	const int required = (int)wcslen(expected) + 1;
	checkFormat(function, 0x007f, 0, value, picture, 0, FALSE, required, 0x4321, NULL);
	checkFormat(function, 0x007f, 0, value, picture, 0, TRUE, required, 0x4321, NULL);
	checkFormat(function, 0x007f, 0, value, picture, required, TRUE, required, 0x4321, expected);
	checkFormat(function, 0x007f, 0, value, picture, 64, TRUE, required, 0x4321, expected);
	checkFormat(function, 0x007f, 0, value, picture, 1, TRUE, 0, ERROR_INSUFFICIENT_BUFFER, L"");
	WCHAR partial[64];
	memcpy(partial, expected, 2 * sizeof(WCHAR));
	partial[2] = 0;
	checkFormat(function, 0x007f, 0, value, picture, 3, TRUE, 0, ERROR_INSUFFICIENT_BUFFER, partial);
	memcpy(partial, expected, (size_t)(required - 2) * sizeof(WCHAR));
	partial[required - 2] = 0;
	checkFormat(function, 0x007f, 0, value, picture, required - 1, TRUE, 0, ERROR_INSUFFICIENT_BUFFER, partial);
	checkFormat(function, 0x007f, 0, value, picture, required, FALSE, 0, ERROR_INVALID_PARAMETER, NULL);
	checkFormat(function, 0x007f, 0, value, picture, -1, TRUE, 0, ERROR_INVALID_PARAMETER, NULL);
	checkFormat(function, 0xffffffff, 0, value, picture, 64, TRUE, 0, ERROR_INVALID_PARAMETER, NULL);
	checkFormat(function, 0x007f, LOCALE_NOUSEROVERRIDE, value, picture, 64, TRUE, 0, ERROR_INVALID_FLAGS, NULL);
	checkFormat(function, 0x007f, 0, value, L"", 64, TRUE, 1, 0x4321, L"");
	checkFormat(function, 0x007f, 0, value, L"'fixed''value'", 64, TRUE, 12, 0x4321, L"fixed'value");
	checkFormat(function, 0x007f, 0, NULL, L"'fixed'", 64, TRUE, 6, 0x4321, L"fixed");
}

static void checkFailures(FormatFunction date, FormatFunction time, const SYSTEMTIME *value, const char *mode) {
	const DWORD error = strcmp(mode, "failed") == 0		   ? ERROR_ACCESS_DENIED
						: strcmp(mode, "unavailable") == 0 ? ERROR_NOT_SUPPORTED
														   : ERROR_INVALID_DATA;
	checkFormat(date, 0x007f, 0, value, L"yyyy-MM-dd", 64, TRUE, 0, error, NULL);
	checkFormat(time, 0x007f, 0, value, L"HH:mm:ss", 64, TRUE, 0, error, NULL);
}

int main(void) {
	HMODULE module = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(module != NULL);
	FormatFunction date, time;
	loadProcedure(module, "GetDateFormatW", &date);
	loadProcedure(module, "GetTimeFormatW", &time);
	SYSTEMTIME value = {2024, 2, 6, 29, 13, 5, 9, 456};
	char mode[32] = {0};
	const DWORD modeLength = GetEnvironmentVariableA("WIBO_FIXTURE_NLS_FORMAT_RESPONSE", mode, sizeof(mode));
	TEST_CHECK(modeLength < sizeof(mode));
	if (modeLength && strcmp(mode, "success") != 0) {
		checkFailures(date, time, &value, mode);
		if (strcmp(mode, "unavailable") == 0)
			return 0;
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_NLS_FORMAT_RESPONSE", "success"));
		checkFormat(date, 0x007f, 0, &value, L"yyyy-MM-dd", 3, TRUE, 0, ERROR_INSUFFICIENT_BUFFER, L"20");
		checkFormat(time, 0x007f, 0, &value, L"HH:mm:ss", 3, TRUE, 0, ERROR_INSUFFICIENT_BUFFER, L"13");
		return 0;
	}
	checkCommon(date, &value, L"yyyy-MM-dd", L"2024-02-29");
	checkCommon(time, &value, L"HH:mm:ss", L"13:05:09");
	checkFormat(date, 0x007f, LOCALE_NOUSEROVERRIDE, &value, NULL, 64, TRUE, 11, 0x4321, L"02/29/2024");
	checkFormat(time, 0x007f, LOCALE_NOUSEROVERRIDE, &value, NULL, 64, TRUE, 9, 0x4321, L"13:05:09");
	checkFormat(date, 0x0409, 0, &value, L"dddd, MMMM d, yyyy", 64, TRUE, 28, 0x4321, L"Thursday, February 29, 2024");
	checkFormat(date, 0x0411, 0, &value, L"yyyy'\x5e74'M'\x6708'd'\x65e5'", 64, TRUE, 11, 0x4321,
				L"2024\x5e74"
				L"2\x6708"
				L"29\x65e5");
	checkFormat(date, 0x0409, DATE_SHORTDATE | LOCALE_NOUSEROVERRIDE, &value, NULL, 64, TRUE, 10, 0x4321, L"2/29/2024");
	checkFormat(date, 0x0409, DATE_LONGDATE | LOCALE_NOUSEROVERRIDE, &value, NULL, 64, TRUE, 28, 0x4321,
				L"Thursday, February 29, 2024");
	checkFormat(date, 0x0409, DATE_YEARMONTH | LOCALE_NOUSEROVERRIDE, &value, NULL, 64, TRUE, 14, 0x4321,
				L"February 2024");
	checkFormat(date, 0x0409, DATE_SHORTDATE | DATE_LONGDATE, &value, NULL, 64, TRUE, 0, ERROR_INVALID_FLAGS, NULL);
	checkFormat(date, 0x0409, DATE_SHORTDATE, &value, L"yyyy-MM-dd", 64, TRUE, 0, ERROR_INVALID_FLAGS, NULL);
	SYSTEMTIME variant = value;
	variant.wDay = 30;
	checkFormat(date, 0x007f, 0, &variant, L"yyyy-MM-dd", 64, TRUE, 0, ERROR_INVALID_PARAMETER, NULL);
	variant = value;
	variant.wHour = variant.wMinute = variant.wSecond = variant.wMilliseconds = 65535;
	checkFormat(date, 0x007f, 0, &variant, L"yyyy-MM-dd", 64, TRUE, 11, 0x4321, L"2024-02-29");
	variant = value;
	variant.wDayOfWeek = 65535;
	checkFormat(date, 0x0409, 0, &variant, L"dddd", 64, TRUE, 9, 0x4321, L"Thursday");
	checkFormat(time, 0x0409, 0, &value, L"hh:mm:ss tt", 64, TRUE, 12, 0x4321, L"01:05:09 PM");
	checkFormat(time, 0x0409, TIME_NOSECONDS, &value, L"HH:mm:ss", 64, TRUE, 6, 0x4321, L"13:05");
	checkFormat(time, 0x0409, TIME_NOMINUTESORSECONDS, &value, L"HH:mm:ss", 64, TRUE, 3, 0x4321, L"13");
	checkFormat(time, 0x0409, TIME_NOTIMEMARKER, &value, L"hh:mm:ss tt", 64, TRUE, 9, 0x4321, L"01:05:09");
	checkFormat(time, 0x0409, TIME_FORCE24HOURFORMAT, &value, L"hh:mm:ss tt", 64, TRUE, 12, 0x4321, L"13:05:09 PM");
	variant = value;
	variant.wHour = 24;
	checkFormat(time, 0x007f, 0, &variant, L"HH:mm:ss", 64, TRUE, 0, ERROR_INVALID_PARAMETER, NULL);
	variant = value;
	variant.wMilliseconds = 1000;
	checkFormat(time, 0x007f, 0, &variant, L"HH:mm:ss", 64, TRUE, 0, ERROR_INVALID_PARAMETER, NULL);
	variant = value;
	variant.wYear = variant.wMonth = variant.wDay = variant.wDayOfWeek = 65535;
	checkFormat(time, 0x007f, 0, &variant, L"HH:mm:ss", 64, TRUE, 9, 0x4321, L"13:05:09");
	puts("NLS format fixture complete");
	return 0;
}
