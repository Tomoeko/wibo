#include "test_assert.h"
#include <windows.h>

typedef ULONG(WINAPI *ConvertStatus)(LONG);
int main(void) {
	HMODULE module = LoadLibraryA("ntdll.dll");
	TEST_CHECK(module != NULL);
	ConvertStatus convert = (ConvertStatus)(void *)GetProcAddress(module, "RtlNtStatusToDosError");
	TEST_CHECK(convert != NULL);
	const struct {
		ULONG status;
		ULONG error;
	} cases[] = {{0, 0},
				 {0xc0000022U, ERROR_ACCESS_DENIED},
				 {0xc0000008U, ERROR_INVALID_HANDLE},
				 {0xc000000dU, ERROR_INVALID_PARAMETER},
				 {0x80000005U, ERROR_MORE_DATA},
				 {0x20000001U, 0x20000001U},
				 {0xd0000022U, ERROR_ACCESS_DENIED},
				 {0xc0070005U, ERROR_ACCESS_DENIED},
				 {0xc0123456U, ERROR_MR_MID_NOT_FOUND}};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		SetLastError(0x71);
		TEST_CHECK_EQ(cases[i].error, convert((LONG)cases[i].status));
		TEST_CHECK_EQ(0x71, GetLastError());
	}
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_STATUS_RESPONSE", "truncated"));
		SetLastError(0x71);
		TEST_CHECK_EQ(ERROR_ACCESS_DENIED, convert((LONG)0xc0000022U));
		TEST_CHECK_EQ(0x71, GetLastError());
		TEST_CHECK_EQ(ERROR_MR_MID_NOT_FOUND, convert((LONG)0xc0987654U));
		TEST_CHECK_EQ(ERROR_INVALID_DATA, GetLastError());
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_STATUS_RESPONSE", NULL));
	}
	TEST_CHECK(FreeLibrary(module));
	return 0;
}
