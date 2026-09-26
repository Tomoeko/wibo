#include "test_assert.h"
#include <windows.h>

int main(void) {
	const int indices[] = {SM_CXSCREEN, SM_CYSCREEN, SM_CXICON, SM_CYICON, SM_CXCURSOR, SM_CYCURSOR, SM_CMONITORS};
	for (unsigned i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
		SetLastError(0x71);
		TEST_CHECK(GetSystemMetrics(indices[i]) > 0);
		if (getenv("WIBO_FIXTURE_PROVIDER"))
			TEST_CHECK_EQ(0x71, GetLastError());
	}
	SetLastError(0x71);
	TEST_CHECK_EQ(0, GetSystemMetrics(-1));
	if (getenv("WIBO_FIXTURE_PROVIDER"))
		TEST_CHECK_EQ(0x71, GetLastError());
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK_EQ(1280, GetSystemMetrics(SM_CXSCREEN));
		TEST_CHECK_EQ(720, GetSystemMetrics(SM_CYSCREEN));
		TEST_CHECK_EQ(-1280, GetSystemMetrics(SM_XVIRTUALSCREEN));
		const char *faults[] = {"truncated", "trailing", "failed"};
		for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
			TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_METRICS_RESPONSE", faults[i]));
			TEST_CHECK_EQ(0, GetSystemMetrics(SM_CXSCREEN));
			TEST_CHECK_EQ(i == 2 ? ERROR_ACCESS_DENIED : ERROR_INVALID_DATA, GetLastError());
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_METRICS_RESPONSE", NULL));
	}
	return 0;
}
