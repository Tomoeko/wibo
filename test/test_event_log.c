#include <windows.h>

#include "test_assert.h"

int main(int argc, char **argv) {
	SetLastError(0x4321);
	HANDLE source = RegisterEventSourceW(NULL, L"wibo-fixture-source");
	TEST_CHECK(source != NULL);
	const WCHAR *strings[] = {L"sample text"};
	const BYTE data[] = {0x11, 0x22, 0x33};
	if (argc > 1 && strcmp(argv[1], "no-sink") == 0) {
		TEST_CHECK(
			!ReportEventW(source, EVENTLOG_INFORMATION_TYPE, 2, 17, NULL, 1, sizeof(data), strings, (LPVOID)data));
		TEST_CHECK_EQ(ERROR_NOT_SUPPORTED, GetLastError());
		TEST_CHECK(DeregisterEventSource(source));
		return 0;
	}
	TEST_CHECK(ReportEventW(source, EVENTLOG_INFORMATION_TYPE, 2, 17, NULL, 1, sizeof(data), strings, (LPVOID)data));
	TEST_CHECK(DeregisterEventSource(source));
	return 0;
}
