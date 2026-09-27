#include <windows.h>

#include "test_assert.h"

static unsigned callbackCount;

static LONG CALLBACK handler(PEXCEPTION_POINTERS info) {
	(void)info;
	++callbackCount;
	return EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
	PVOID registrations[128];
	SetLastError(0x20001234);
	for (unsigned i = 0; i < sizeof(registrations) / sizeof(registrations[0]); ++i) {
		registrations[i] = AddVectoredExceptionHandler(i & 1 ? 99 : 0, handler);
		TEST_CHECK(registrations[i] != NULL);
		TEST_CHECK_EQ(0x20001234, GetLastError());
		for (unsigned j = 0; j < i; ++j) {
			TEST_CHECK(registrations[i] != registrations[j]);
		}
	}
	for (unsigned i = 0; i < sizeof(registrations) / sizeof(registrations[0]); ++i) {
		TEST_CHECK(RemoveVectoredExceptionHandler(registrations[i]) != 0);
		TEST_CHECK_EQ(0x20001234, GetLastError());
		TEST_CHECK_EQ(0, RemoveVectoredExceptionHandler(registrations[i]));
		TEST_CHECK_EQ(0x20001234, GetLastError());
	}
	TEST_CHECK_EQ(0, RemoveVectoredExceptionHandler(NULL));
	TEST_CHECK_EQ(0x20001234, GetLastError());
	TEST_CHECK_EQ(0, callbackCount);
	return 0;
}
