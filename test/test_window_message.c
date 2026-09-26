#include "test_assert.h"
#include <windows.h>

int main(void) {
	UINT first = RegisterWindowMessageA("wibo.fixture.message.first");
	UINT second = RegisterWindowMessageW(L"wibo.fixture.message.second");
	TEST_CHECK(first >= 0xC000 && first <= 0xFFFF);
	TEST_CHECK(second >= 0xC000 && second <= 0xFFFF);
	TEST_CHECK(first != second);
	TEST_CHECK_EQ(first, RegisterWindowMessageW(L"wibo.fixture.message.first"));
	TEST_CHECK_EQ(first, RegisterWindowMessageW(L"WIBO.FIXTURE.MESSAGE.FIRST"));
	TEST_CHECK_EQ(second, RegisterWindowMessageA("wibo.fixture.message.second"));
	TEST_CHECK_EQ(first, RegisterClipboardFormatA("wibo.fixture.message.first"));
	UINT format = RegisterClipboardFormatW(L"wibo.fixture.format");
	TEST_CHECK(format >= 0xC000 && format <= 0xFFFF);
	TEST_CHECK_EQ(format, RegisterClipboardFormatA("WIBO.FIXTURE.FORMAT"));
	TEST_CHECK_EQ(format, RegisterWindowMessageW(L"wibo.fixture.format"));
	TEST_CHECK_EQ(0, RegisterWindowMessageA(""));
	return 0;
}
