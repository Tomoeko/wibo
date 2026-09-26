#include <winsock2.h>
#include <ws2ipdef.h>

#include "test_assert.h"
#include <iphlpapi.h>
#include <netioapi.h>

int main(void) {
	_Static_assert(sizeof(SOCKADDR_INET) == 28, "Socket address ABI");
	_Static_assert(sizeof(MIB_IPFORWARD_ROW2) == 104, "Route ABI");
	SOCKADDR_INET destination = {0}, source = {0};
	destination.Ipv4.sin_family = AF_INET;
	const BYTE loopback[4] = {127, 0, 0, 1};
	memcpy(&destination.Ipv4.sin_addr, loopback, sizeof(loopback));
	MIB_IPFORWARD_ROW2 route;
	memset(&route, 0xcc, sizeof(route));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetBestRoute2(NULL, 0, NULL, NULL, 0, &route, &source));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetBestRoute2(NULL, 0, NULL, &destination, 0, NULL, &source));
	TEST_CHECK_EQ(ERROR_INVALID_PARAMETER, GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, NULL));
	SetLastError(0x731);
	TEST_CHECK_EQ(NO_ERROR, GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, &source));
	if (getenv("WIBO_FIXTURE_RUNTIME"))
		TEST_CHECK_EQ(0x731, GetLastError());
	TEST_CHECK_EQ(AF_INET, source.si_family);
	TEST_CHECK(memcmp(&source.Ipv4.sin_addr, loopback, sizeof(loopback)) == 0);
	TEST_CHECK(route.InterfaceIndex != 0);
	TEST_CHECK_EQ(AF_INET, route.DestinationPrefix.Prefix.si_family);
	TEST_CHECK(route.DestinationPrefix.PrefixLength <= 32);
	NET_LUID luid = route.InterfaceLuid;
	TEST_CHECK_EQ(NO_ERROR, GetBestRoute2(&luid, 0, &source, &destination, 0, &route, &source));
	TEST_CHECK_EQ(luid.Value, route.InterfaceLuid.Value);
	if (getenv("WIBO_FIXTURE_PROVIDER")) {
		const char *faults[] = {"truncated", "trailing", "bad-size", "failed"};
		for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
			TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_ROUTE_RESPONSE", faults[i]));
			memset(&route, 0xcc, sizeof(route));
			memset(&source, 0xcc, sizeof(source));
			TEST_CHECK_EQ(i == 3 ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_DATA,
						  GetBestRoute2(NULL, 0, NULL, &destination, 0, &route, &source));
			for (unsigned j = 0; j < sizeof(route); ++j)
				TEST_CHECK_EQ(0xcc, ((BYTE *)&route)[j]);
			for (unsigned j = 0; j < sizeof(source); ++j)
				TEST_CHECK_EQ(0xcc, ((BYTE *)&source)[j]);
		}
		TEST_CHECK(SetEnvironmentVariableA("WIBO_FIXTURE_ROUTE_RESPONSE", NULL));
	}
	return 0;
}
