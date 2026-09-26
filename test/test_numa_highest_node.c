#define _WIN32_WINNT 0x0600
#include <windows.h>

#include "test_assert.h"

int main(void) {
	ULONG nodes[3] = {0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5};
	const char *fault = getenv("WIBO_FIXTURE_NUMA_RESPONSE");
	SetLastError(0x20001234);
	BOOL result = GetNumaHighestNodeNumber(&nodes[1]);
	TEST_CHECK_EQ(0xa5a5a5a5, nodes[0]);
	TEST_CHECK_EQ(0xa5a5a5a5, nodes[2]);
	if (fault) {
		TEST_CHECK(!result);
		TEST_CHECK_EQ(!strcmp(fault, "failed")		  ? ERROR_ACCESS_DENIED
					  : !strcmp(fault, "unavailable") ? ERROR_NOT_SUPPORTED
													  : ERROR_INVALID_DATA,
					  GetLastError());
		TEST_CHECK_EQ(0xa5a5a5a5, nodes[1]);
	} else {
		TEST_CHECK(result);
		TEST_CHECK_EQ(0x20001234, GetLastError());
		TEST_CHECK(nodes[1] != 0xa5a5a5a5);
		if (getenv("WIBO_FIXTURE_PROVIDER"))
			TEST_CHECK_EQ(3, nodes[1]);
	}
	return 0;
}
