#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

#include "test_assert.h"

static void test_getlocaleinfow(LCTYPE type) {
	LCID lcid = GetUserDefaultLCID();
	int required_w = GetLocaleInfoW(lcid, type, NULL, 0);
	TEST_CHECK(required_w > 0);

	WCHAR *buffer_w = (WCHAR *)malloc((size_t)required_w * sizeof(WCHAR));
	TEST_CHECK(buffer_w != NULL);
	int written_w = GetLocaleInfoW(lcid, type, buffer_w, required_w);
	TEST_CHECK(written_w > 0);

	TEST_CHECK_EQ(required_w, written_w);
	TEST_CHECK_EQ(required_w - 1, wcslen(buffer_w));

	free(buffer_w);
}

static void test_getlocaleinfow_errors(void) {
	WCHAR buffer[16];

	SetLastError(0);
	TEST_CHECK(!GetLocaleInfoW(GetUserDefaultLCID(), LOCALE_SENGCOUNTRY, buffer, -1));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetLastError());

	SetLastError(0);
	TEST_CHECK(!GetLocaleInfoW(GetUserDefaultLCID(), LOCALE_SENGCOUNTRY, buffer, 1));
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetLastError());
}

int main(void) {
	test_getlocaleinfow(LOCALE_SENGCOUNTRY);
	test_getlocaleinfow(LOCALE_SENGLANGUAGE);
	test_getlocaleinfow_errors();
	return 0;
}
