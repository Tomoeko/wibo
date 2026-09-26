#include "test_assert.h"
#include <windows.h>
#include <winsock2.h>

int main(void) {
	WSADATA data;
	TEST_CHECK_EQ(0, WSAStartup(MAKEWORD(2, 2), &data));
	GUID type = {0x0002a800, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
	WSAQUERYSETW query = {0};
	query.dwSize = sizeof(query);
	query.lpServiceClassId = &type;
	AFPROTOCOLS protocols[2] = {{AF_INET, IPPROTO_UDP}, {AF_INET, IPPROTO_TCP}};
	query.dwNumberOfProtocols = 2;
	query.lpafpProtocols = protocols;
	HANDLE lookup = (HANDLE)0x71;
	const int result = WSALookupServiceBeginW(&query, LUP_RETURN_NAME | LUP_RETURN_TYPE, &lookup);
	if (result == SOCKET_ERROR && !getenv("WIBO_FIXTURE_RUNTIME")) {
		// The compatibility provider has no local host-name service implementation.
		TEST_CHECK_EQ(WSA_NOT_ENOUGH_MEMORY, WSAGetLastError());
		TEST_CHECK_EQ(0x71, (ULONG_PTR)lookup);
		printf("Local host-name result checks unavailable in the compatibility provider\n");
		TEST_CHECK_EQ(0, WSACleanup());
		return 0;
	}
	TEST_CHECK_EQ(0, result);
	TEST_CHECK(lookup != NULL && lookup != INVALID_HANDLE_VALUE);
	union {
		ULONGLONG align;
		BYTE bytes[2048];
	} buffer;
	memset(buffer.bytes, 0xcc, sizeof(buffer.bytes));
	DWORD length = sizeof(WSAQUERYSETW) - 1;
	TEST_CHECK_EQ(SOCKET_ERROR, WSALookupServiceNextW(lookup, 0, &length, (WSAQUERYSETW *)buffer.bytes));
	TEST_CHECK_EQ(WSAEFAULT, WSAGetLastError());
	TEST_CHECK(length > sizeof(WSAQUERYSETW) && length < sizeof(buffer.bytes) - 8);
	for (unsigned i = 0; i < sizeof(buffer.bytes); ++i)
		TEST_CHECK_EQ(0xcc, buffer.bytes[i]);
	DWORD needed = length;
	TEST_CHECK_EQ(0, WSALookupServiceNextW(lookup, 0, &length, (WSAQUERYSETW *)buffer.bytes));
	WSAQUERYSETW *entry = (WSAQUERYSETW *)buffer.bytes;
	TEST_CHECK_EQ(sizeof(*entry), entry->dwSize);
	TEST_CHECK_EQ(NS_DNS, entry->dwNameSpace);
	TEST_CHECK(entry->lpServiceClassId != NULL);
	TEST_CHECK(memcmp(&type, entry->lpServiceClassId, sizeof(type)) == 0);
	TEST_CHECK((BYTE *)entry->lpszServiceInstanceName >= buffer.bytes + sizeof(*entry));
	TEST_CHECK((BYTE *)entry->lpszServiceInstanceName < buffer.bytes + needed);
	char hostname[256];
	TEST_CHECK_EQ(0, gethostname(hostname, sizeof(hostname)));
	WCHAR expected[256];
	TEST_CHECK(MultiByteToWideChar(CP_ACP, 0, hostname, -1, expected, 256) > 0);
	TEST_CHECK(wcscmp(expected, entry->lpszServiceInstanceName) == 0);
	for (unsigned i = needed; i < sizeof(buffer.bytes); ++i)
		TEST_CHECK_EQ(0xcc, buffer.bytes[i]);
	TEST_CHECK_EQ(SOCKET_ERROR, WSALookupServiceNextW(lookup, 0, &length, entry));
	TEST_CHECK_EQ(WSA_E_NO_MORE, WSAGetLastError());
	TEST_CHECK_EQ(0, WSALookupServiceEnd(lookup));
	TEST_CHECK_EQ(SOCKET_ERROR, WSALookupServiceEnd(lookup));
	TEST_CHECK_EQ(WSA_INVALID_HANDLE, WSAGetLastError());
	TEST_CHECK_EQ(SOCKET_ERROR, WSALookupServiceNextW(lookup, 0, &length, entry));
	TEST_CHECK_EQ(WSA_INVALID_HANDLE, WSAGetLastError());
	TEST_CHECK_EQ(0, WSALookupServiceBeginW(&query, LUP_RETURN_NAME, &lookup));
	length = sizeof(buffer.bytes);
	TEST_CHECK_EQ(SOCKET_ERROR, WSALookupServiceNextW(lookup, LUP_FLUSHPREVIOUS, &length, entry));
	TEST_CHECK_EQ(WSA_E_NO_MORE, WSAGetLastError());
	TEST_CHECK_EQ(0, WSALookupServiceEnd(lookup));
	TEST_CHECK_EQ(0, WSACleanup());
	return 0;
}
