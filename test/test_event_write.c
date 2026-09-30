#include <windows.h>

#include <evntprov.h>

#include "test_assert.h"

int main(void) {
	const GUID provider = {0x19f53b26, 0x4821, 0x40bc, {0x9a, 0x36, 0xa8, 0x81, 0x72, 0x4d, 0x6c, 0x10}};
	REGHANDLE handle = 0;
	TEST_CHECK_EQ(ERROR_SUCCESS, EventRegister(&provider, NULL, NULL, &handle));
	TEST_CHECK(handle != 0);

	EVENT_DESCRIPTOR event = {0};
	event.Id = 1;
	event.Level = 4;
	event.Keyword = 1;
	const BYTE value = 7;
	EVENT_DATA_DESCRIPTOR data = {0};
	data.Ptr = (ULONGLONG)(ULONG_PTR)&value;
	data.Size = sizeof(value);

	TEST_CHECK_EQ(ERROR_SUCCESS, EventWrite(0, NULL, 0, NULL));
	TEST_CHECK_EQ(ERROR_SUCCESS, EventWrite(handle, &event, 1, &data));
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(handle));
	TEST_CHECK_EQ(ERROR_INVALID_HANDLE, EventWrite(handle, &event, 1, &data));
	TEST_CHECK_EQ(ERROR_SUCCESS, EventUnregister(0));
	return 0;
}
