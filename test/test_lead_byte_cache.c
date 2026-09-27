#include <stdio.h>
#include <windows.h>

#include "test_assert.h"

int main(void) {
	char mode[32] = {0};
	const DWORD length = GetEnvironmentVariableA("WIBO_FIXTURE_LEAD_RESPONSE", mode, sizeof(mode));
	TEST_CHECK(length && length < sizeof(mode));
	if (strcmp(mode, "success") != 0) {
		SetLastError(0x4321);
		TEST_CHECK_EQ(FALSE, IsDBCSLeadByteEx(60000, 0x81));
		TEST_CHECK_EQ(strcmp(mode, "failed") == 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_DATA, GetLastError());
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_LEAD_RESPONSE", "success"));
	}
	SetLastError(0x4321);
	TEST_CHECK_EQ(TRUE, IsDBCSLeadByteEx(60000, 0x81));
	TEST_CHECK_EQ(0x4321, GetLastError());
	TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_LEAD_RESPONSE", "failed"));
	const unsigned int bytes[] = {0, 0x80, 0x81, 0x9f, 0xa0, 0xff};
	for (unsigned int index = 0; index < sizeof(bytes) / sizeof(bytes[0]); ++index) {
		SetLastError(0x4321);
		TEST_CHECK_EQ(bytes[index] >= 0x81 && bytes[index] <= 0x9f, IsDBCSLeadByteEx(60000, (BYTE)bytes[index]));
		TEST_CHECK_EQ(0x4321, GetLastError());
	}
	puts("Synthetic lead-byte metadata retry and cache verified");
	return 0;
}
