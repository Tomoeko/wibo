#include <winsock2.h>

#include "test_assert.h"
#include <iphlpapi.h>
#include <stddef.h>

static BYTE *start;
static ULONG capacity;
static void within(const void *pointer, size_t bytes) {
	TEST_CHECK((const BYTE *)pointer >= start);
	TEST_CHECK((const BYTE *)pointer <= start + capacity);
	TEST_CHECK(bytes <= (size_t)(start + capacity - (const BYTE *)pointer));
}

int main(void) {
	ULONG size = 0;
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetAdaptersAddresses(99, 0, NULL, NULL, &size));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, NULL));
	for (unsigned f = 0; f < 3; ++f) {
		ULONG family = f == 0 ? AF_UNSPEC : f == 1 ? AF_INET : AF_INET6;
		ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS;
		size = 0;
		DWORD result = GetAdaptersAddresses(family, flags, NULL, NULL, &size);
		if (result == ERROR_NO_DATA)
			continue;
		TEST_CHECK_EQ(ERROR_BUFFER_OVERFLOW, result);
		TEST_CHECK(size >= offsetof(IP_ADAPTER_ADDRESSES, FirstPrefix) + sizeof(void *));
		capacity = size;
		start = malloc((size_t)capacity + 16);
		TEST_CHECK(start != NULL);
		memset(start, 0xcc, (size_t)capacity + 16);
		ULONG small = 1;
		TEST_CHECK_EQ(ERROR_BUFFER_OVERFLOW, GetAdaptersAddresses(family, flags, NULL, (void *)start, &small));
		for (ULONG i = 0; i < capacity + 16; ++i)
			TEST_CHECK_EQ(0xcc, start[i]);
		SetLastError(0x731);
		for (unsigned retry = 0; retry < 3; ++retry) {
			result = GetAdaptersAddresses(family, flags, NULL, (void *)start, &size);
			if (result != ERROR_BUFFER_OVERFLOW)
				break;
			for (ULONG i = capacity; i < capacity + 16; ++i)
				TEST_CHECK_EQ(0xcc, start[i]);
			free(start);
			capacity = size;
			start = malloc((size_t)capacity + 16);
			TEST_CHECK(start != NULL);
			memset(start, 0xcc, (size_t)capacity + 16);
		}
		TEST_CHECK_EQ(NO_ERROR, result);
		TEST_CHECK_EQ(0x731, GetLastError());
		unsigned adapters = 0, addresses = 0;
		for (IP_ADAPTER_ADDRESSES *entry = (void *)start; entry; entry = entry->Next) {
			TEST_CHECK(++adapters < 4096);
			within(entry, entry->Length);
			TEST_CHECK(entry->PhysicalAddressLength <= sizeof(entry->PhysicalAddress));
			within(entry->AdapterName, strlen(entry->AdapterName) + 1);
			if (entry->FriendlyName)
				within(entry->FriendlyName, (wcslen(entry->FriendlyName) + 1) * sizeof(WCHAR));
			for (IP_ADAPTER_UNICAST_ADDRESS *address = entry->FirstUnicastAddress; address; address = address->Next) {
				TEST_CHECK(++addresses < 65536);
				within(address, address->Length);
				within(address->Address.lpSockaddr, address->Address.iSockaddrLength);
				USHORT af = address->Address.lpSockaddr->sa_family;
				TEST_CHECK(af == AF_INET || af == AF_INET6);
				TEST_CHECK(family == AF_UNSPEC || af == family);
			}
			for (IP_ADAPTER_PREFIX *prefix = entry->FirstPrefix; prefix; prefix = prefix->Next) {
				within(prefix, prefix->Length);
				within(prefix->Address.lpSockaddr, prefix->Address.iSockaddrLength);
				TEST_CHECK(prefix->PrefixLength <= (prefix->Address.lpSockaddr->sa_family == AF_INET ? 32 : 128));
			}
		}
		TEST_CHECK(adapters > 0);
		for (ULONG i = capacity; i < capacity + 16; ++i)
			TEST_CHECK_EQ(0xcc, start[i]);
		free(start);
	}
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		const char *faults[] = {"truncated", "trailing", "outside", "overlap", "width", "failed"};
		for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
			TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_ADAPTER_RESPONSE", faults[i]));
			size = 0x71;
			TEST_CHECK_EQ(i == 5 ? ERROR_ACCESS_DENIED : ERROR_INVALID_DATA,
						  GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &size));
			TEST_CHECK_EQ(0x71, size);
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_ADAPTER_RESPONSE", NULL));
	}
	return 0;
}
