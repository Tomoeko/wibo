#include "test_assert.h"
#include <windows.h>

#include <evntprov.h>

static unsigned callback_count;

static void WINAPI disabled_callback(LPCGUID id, ULONG enabled, UCHAR level, ULONGLONG any_keyword,
									 ULONGLONG all_keyword, PEVENT_FILTER_DESCRIPTOR filter, PVOID context) {
	(void)id;
	(void)enabled;
	(void)level;
	(void)any_keyword;
	(void)all_keyword;
	(void)filter;
	(void)context;
	++callback_count;
}

int main(void) {
	GUID id = {0x1681d246, 0x5357, 0x4944, {0x91, 0x52, 0x40, 0x31, 0x25, 0x16, 0x73, 0x02}};
	id.Data1 ^= GetCurrentProcessId();
	struct {
		ULONGLONG before;
		REGHANDLE handle;
		ULONGLONG after;
	} first = {0x1122334455667788ULL, 0, 0x1122334455667788ULL};
	SetLastError(0x4321);
	TEST_CHECK_EQ(ERROR_SUCCESS, EventRegister(&id, disabled_callback, NULL, &first.handle));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK(first.handle != 0);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, first.before);
	TEST_CHECK_U64_EQ(0x1122334455667788ULL, first.after);
	REGHANDLE second = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, EventRegister(&id, NULL, NULL, &second));
	TEST_CHECK(second != 0 && second != first.handle);
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(first.handle));
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(second));
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(0));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK_EQ(0, callback_count);
	return 0;
}
