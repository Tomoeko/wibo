#include "test_assert.h"
#include <winsock2.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	WSAEVENT events[2] = {WSACreateEvent(), WSACreateEvent()};
	TEST_CHECK(events[0] != WSA_INVALID_EVENT && events[1] != WSA_INVALID_EVENT);
	TEST_CHECK_EQ(WSA_WAIT_TIMEOUT, WSAWaitForMultipleEvents(2, events, FALSE, 0, FALSE));
	TEST_CHECK(WSASetEvent(events[1]));
	TEST_CHECK_EQ(WSA_WAIT_EVENT_0 + 1, WSAWaitForMultipleEvents(2, events, FALSE, 0, FALSE));
	TEST_CHECK_EQ(WSA_WAIT_EVENT_0 + 1, WSAWaitForMultipleEvents(2, events, FALSE, 0, FALSE));
	TEST_CHECK_EQ(WSA_WAIT_TIMEOUT, WSAWaitForMultipleEvents(2, events, TRUE, 0, FALSE));
	TEST_CHECK(WSASetEvent(events[0]));
	TEST_CHECK_EQ(WSA_WAIT_EVENT_0, WSAWaitForMultipleEvents(2, events, TRUE, 0, FALSE));
	TEST_CHECK(WSAResetEvent(events[0]));
	TEST_CHECK_EQ(WSA_WAIT_EVENT_0 + 1, WSAWaitForMultipleEvents(2, events, FALSE, 0, FALSE));
	TEST_CHECK(WSACloseEvent(events[0]));
	TEST_CHECK(WSACloseEvent(events[1]));
	TEST_CHECK(!WSACloseEvent(events[1]));
	TEST_CHECK_EQ(WSA_INVALID_HANDLE, WSAGetLastError());
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
