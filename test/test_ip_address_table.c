#include "test_assert.h"
#include <iphlpapi.h>
#include <stddef.h>
#include <stdlib.h>
#include <winsock2.h>

int main(void) {
	TEST_CHECK_EQ(24, sizeof(MIB_IPADDRROW));
	TEST_CHECK_EQ(4, offsetof(MIB_IPADDRTABLE, table));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetIpAddrTable(NULL, NULL, FALSE));
	ULONG size = 0;
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetIpAddrTable(NULL, &size, FALSE));
	TEST_CHECK(size >= sizeof(DWORD));
	BYTE *buffer = malloc(size + sizeof(DWORD));
	TEST_CHECK(buffer != NULL);
	ULONG capacity = size;
	memset(buffer, 0x55, size + sizeof(DWORD));
	TEST_CHECK_EQ(NO_ERROR, GetIpAddrTable((MIB_IPADDRTABLE *)buffer, &size, TRUE));
	MIB_IPADDRTABLE *table = (MIB_IPADDRTABLE *)buffer;
	TEST_CHECK(table->dwNumEntries > 0);
	TEST_CHECK(offsetof(MIB_IPADDRTABLE, table) + table->dwNumEntries * sizeof(MIB_IPADDRROW) <= capacity);
	for (DWORD i = 0; i != table->dwNumEntries; ++i) {
		TEST_CHECK(table->table[i].dwIndex != 0);
		if (i)
			TEST_CHECK(ntohl(table->table[i - 1].dwAddr) <= ntohl(table->table[i].dwAddr));
	}
	for (ULONG i = capacity; i != capacity + sizeof(DWORD); ++i)
		TEST_CHECK_EQ(0x55, buffer[i]);
	size = 1;
	TEST_CHECK_EQ(ERROR_INSUFFICIENT_BUFFER, GetIpAddrTable(table, &size, FALSE));
	TEST_CHECK(size >= capacity);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		memset(buffer, 0x55, capacity);
		size = capacity;
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_IP_TABLE_RESPONSE", "truncated"));
		TEST_CHECK_EQ(ERROR_INVALID_DATA, GetIpAddrTable(table, &size, FALSE));
		TEST_CHECK_EQ(capacity, size);
		TEST_CHECK_EQ(0x55, buffer[0]);
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_IP_TABLE_RESPONSE", "trailing"));
		TEST_CHECK_EQ(ERROR_INVALID_DATA, GetIpAddrTable(table, &size, FALSE));
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_IP_TABLE_RESPONSE", "bad-count"));
		TEST_CHECK_EQ(ERROR_INVALID_DATA, GetIpAddrTable(table, &size, FALSE));
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_IP_TABLE_RESPONSE", "failed"));
		TEST_CHECK_EQ(ERROR_ACCESS_DENIED, GetIpAddrTable(table, &size, FALSE));
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_IP_TABLE_RESPONSE", NULL));
	}
	free(buffer);
	return 0;
}
