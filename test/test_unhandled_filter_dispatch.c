#include <windows.h>

#include "test_assert.h"

#include <stdio.h>

static volatile LONG callbacks;
static volatile LONG matched;

static LONG WINAPI onUnhandled(EXCEPTION_POINTERS *information) {
	InterlockedIncrement(&callbacks);
	if (information && information->ExceptionRecord && information->ContextRecord &&
		information->ExceptionRecord->ExceptionCode == 0xe0420901u && information->ExceptionRecord->ExceptionFlags == 0)
		InterlockedExchange(&matched, 1);
	return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void) {
	LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(onUnhandled);
	RaiseException(0xe0420901u, 0, 0, NULL);
	SetUnhandledExceptionFilter(previous);
	TEST_CHECK_EQ(1, callbacks);
	TEST_CHECK_EQ(1, matched);
	puts("top_level_filter=continued");
	return 0;
}
